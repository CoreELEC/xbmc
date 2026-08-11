/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/guiinfo/CEGUIInfoRegistry.h"
#include "utils/Base64.h"
#include "utils/TimeUtils.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
}

extern "C"
{
#include "obu_util.h"
}

// Live DV/HDR metadata payloads located in the stream by the Amlogic codec and
// published base64 encoded under the player.process(video.sidedata) label.

struct AMLFrameMetadata
{
  std::string doviConfig;
  std::vector<std::string> flags;
  std::string structure;
  std::string doviRpu;
  std::string hdr10pSei;
  std::string hdrMdcv;
  std::string hdrCll;

  bool operator==(const AMLFrameMetadata&) const = default;

  // a frame without a payload holds the last carried one instead of
  // flickering empty
  void Inherit(const AMLFrameMetadata& prev)
  {
    if (doviRpu.empty())
      doviRpu = prev.doviRpu;
    if (hdr10pSei.empty())
      hdr10pSei = prev.hdr10pSei;
    if (hdrMdcv.empty())
      hdrMdcv = prev.hdrMdcv;
    if (hdrCll.empty())
      hdrCll = prev.hdrCll;
  }

  // values are base64 or plain flag tokens, so no JSON escaping is needed
  std::string ComposeSideData() const
  {
    std::string flagsJson;
    for (const auto& flag : flags)
    {
      flagsJson += flagsJson.empty() ? '[' : ',';
      flagsJson += '"';
      flagsJson += flag;
      flagsJson += '"';
    }
    if (!flagsJson.empty())
      flagsJson += ']';

    const std::pair<const char*, const std::string*> entries[] = {
        {"dovi.config", &doviConfig}, {"structure", &structure},
        {"dovi.rpu", &doviRpu},       {"hdr10plus", &hdr10pSei},
        {"mdcv", &hdrMdcv},           {"cll", &hdrCll}};
    std::string json;
    if (!flagsJson.empty())
    {
      json += "{\"flags\":";
      json += flagsJson;
    }
    for (const auto& [key, value] : entries)
    {
      if (value->empty())
        continue;
      json += json.empty() ? "{\"" : ",\"";
      json += key;
      json += "\":\"";
      json += *value;
      json += '"';
    }
    if (!json.empty())
      json += '}';
    return json;
  }
};

// A stream switch opens the successor codec before the predecessor is closed,
// so clearing is gated on an ownership token instead of happening blindly.
class CAMLFrameMetadataStore
{
public:
  static CAMLFrameMetadataStore& GetInstance()
  {
    static CAMLFrameMetadataStore store;
    return store;
  }

  uint32_t Register()
  {
    std::lock_guard lock(m_lock);
    m_owner = ++m_nextToken;
    m_meta = {};
    return m_owner;
  }

  void Unregister(uint32_t token)
  {
    std::lock_guard lock(m_lock);
    if (m_owner == token)
    {
      m_owner = 0;
      m_meta = {};
    }
  }

  void Publish(uint32_t token, const AMLFrameMetadata& meta)
  {
    std::lock_guard lock(m_lock);
    if (token != 0 && m_owner == token && !(m_meta == meta))
      m_meta = meta;
  }

  AMLFrameMetadata Get() const
  {
    std::lock_guard lock(m_lock);
    return m_meta;
  }

private:
  CAMLFrameMetadataStore() = default;

  mutable std::mutex m_lock;
  AMLFrameMetadata m_meta;
  uint32_t m_owner{0};
  uint32_t m_nextToken{0};
};

// Orders metadata by presentation: committed per pts at decode, released when
// the drain target reaches their pts. All methods run on the VideoPlayerVideo
// thread.
class CAMLFrameMetadataSequencer
{
public:
  void Commit(double pts, const AMLFrameMetadata& meta)
  {
    // a pts far below the newest queued entry means the feed jumped backwards
    // without a flush, and the stranded entries would otherwise win eviction
    if (!m_queue.empty() && pts + BACKWARD_JUMP < m_queue.rbegin()->first)
      m_queue.clear();
    m_queue[pts] = meta;
    if (m_queue.size() > MAX_DEPTH)
      Compact();
    // the oldest entries are the next to be consumed, so overflow drops newest
    while (m_queue.size() > MAX_DEPTH)
      m_queue.erase(std::prev(m_queue.end()));
  }

  // newest entry at or before pts, consuming everything up to it. A miss keeps
  // the queue intact so the caller can hold the last published values.
  bool Consume(double pts, AMLFrameMetadata& meta)
  {
    auto it = m_queue.upper_bound(pts + PTS_TOLERANCE);
    if (it == m_queue.begin())
      return false;
    --it;
    meta = it->second;
    m_queue.erase(m_queue.begin(), std::next(it));
    return true;
  }

  bool Empty() const { return m_queue.empty(); }

  void Reset() { m_queue.clear(); }

private:
  // an entry equal to its pts predecessor can go; the newest stay untouched
  // since a late reordered commit could still land between them
  void Compact()
  {
    if (m_queue.size() <= REORDER_MARGIN)
      return;
    const auto stop = std::prev(m_queue.end(), REORDER_MARGIN);
    for (auto it = std::next(m_queue.begin()); it != stop;)
    {
      if (it->second == std::prev(it)->second)
        it = m_queue.erase(it);
      else
        ++it;
    }
  }

  static constexpr double PTS_TOLERANCE = 1000.0; // DVD_TIME_BASE is microseconds
  static constexpr double BACKWARD_JUMP = 5000000.0;
  // deep enough that after compaction only per frame churn can overflow it
  static constexpr size_t MAX_DEPTH = 512;
  static constexpr size_t REORDER_MARGIN = 64;

  std::map<double, AMLFrameMetadata> m_queue;
};

// one store snapshot per render pass so a consumer cannot mix payloads from
// different video frames, and thread_local avoids a shared cache lock
inline const std::string& AMLGetCachedSideData()
{
  thread_local std::string cached;
  thread_local unsigned int cachedFrameTime = 0;
  thread_local bool cachedOnce = false;

  const unsigned int frameTime = CTimeUtils::GetFrameTime();
  if (!cachedOnce || frameTime != cachedFrameTime)
  {
    cached = CAMLFrameMetadataStore::GetInstance().Get().ComposeSideData();
    cachedFrameTime = frameTime;
    cachedOnce = true;
  }
  return cached;
}

inline bool AMLFrameMetadataGetLabel(std::string& value, int info)
{
  if (static_cast<uint32_t>(info) != CE::GUIINFO::CE_PLAYER_PROCESS_VIDEO_SIDEDATA)
    return false;
  value = AMLGetCachedSideData();
  return true;
}

enum
{
  HEVC_NAL_SEI_PREFIX = 39,
  HEVC_NAL_UNSPEC62 = 62 // Dolby Vision RPU
};

enum
{
  SEI_PAYLOAD_REGISTERED_ITU_T_T35 = 4,
  SEI_PAYLOAD_MASTERING_DISPLAY_COLOUR_VOLUME = 137,
  SEI_PAYLOAD_CONTENT_LIGHT_LEVEL_INFO = 144
};

// the T.35 header every HDR10+ payload starts with: country 0xB5,
// provider 0x003C, provider oriented code 0x0001, application identifier 4
inline bool AMLIsHdr10PlusT35(const uint8_t* data, size_t size)
{
  return size >= 8 && data[0] == 0xb5 && data[1] == 0x00 && data[2] == 0x3c &&
         data[3] == 0x00 && data[4] == 0x01 && data[5] == 0x04;
}

// the payload layout shared by the HEVC SEI, the AV1 metadata OBU and the MKV
// block addition side data
inline void AMLLatchHdr10PlusT35(const uint8_t* data, size_t size, AMLFrameMetadata& meta)
{
  if (data && AMLIsHdr10PlusT35(data, size))
    meta.hdr10pSei =
        Base64::Encode(reinterpret_cast<const char*>(data), static_cast<unsigned int>(size));
}

// Walks one demux packet for the DV RPU (HEVC NAL UNSPEC62). The latched
// payload keeps the escaped NAL including its 7C 01 header, the exact layout
// libdovi and dovi_tool consume. nalLengthSize is 0 for Annex-B input.
inline void AMLLatchHevcDoviRpu(const uint8_t* data,
                                size_t size,
                                int nalLengthSize,
                                AMLFrameMetadata& meta)
{
  if (!data || size < 4)
    return;

  if (nalLengthSize >= 1 && nalLengthSize <= 4)
  {
    size_t pos = 0;
    while (pos + nalLengthSize <= size)
    {
      uint32_t len = 0;
      for (int i = 0; i < nalLengthSize; ++i)
        len = (len << 8) | data[pos + i];
      pos += nalLengthSize;
      if (len == 0 || len > size - pos)
        return;
      if (((data[pos] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
      {
        meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + pos), len);
        return;
      }
      pos += len;
    }
    return;
  }

  // Annex-B with mixed 3- and 4-byte start codes
  size_t nal = SIZE_MAX;
  for (size_t i = 0; i + 2 < size; ++i)
  {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    if (nal != SIZE_MAX && ((data[nal] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
    {
      // a 4-byte start code owns the zero before this prefix, and an RPU never
      // ends in 0x00 (rbsp_trailing_bits), so trailing zeros are not payload
      size_t end = i;
      while (end > nal && data[end - 1] == 0)
        end--;
      meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + nal),
                                    static_cast<unsigned int>(end - nal));
      return;
    }
    nal = i + 3;
    i += 2;
  }
  if (nal != SIZE_MAX && nal < size && ((data[nal] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
  {
    size_t end = size;
    while (end > nal && data[end - 1] == 0)
      end--;
    meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + nal),
                                  static_cast<unsigned int>(end - nal));
  }
}

// Walks one demux packet for the prefix SEI payloads worth publishing: the
// HDR10+ T.35, the mastering display colour volume and the content light
// level, each latched verbatim after unescaping.
inline void AMLLatchHevcSei(const uint8_t* data,
                            size_t size,
                            int nalLengthSize,
                            AMLFrameMetadata& meta)
{
  if (!data || size < 4)
    return;

  const auto parseSeiNal = [&meta](const uint8_t* nal, size_t len)
  {
    if (len < 3 || ((nal[0] >> 1) & 0x3f) != HEVC_NAL_SEI_PREFIX)
      return;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    for (size_t i = 2; i < len; ++i)
    {
      if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3)
      {
        rbsp.push_back(0);
        rbsp.push_back(0);
        i += 2;
      }
      else
        rbsp.push_back(nal[i]);
    }

    size_t p = 0;
    const size_t end = rbsp.size();
    while (p + 2 < end)
    {
      uint32_t type = 0;
      while (p < end && rbsp[p] == 0xFF)
      {
        type += 255;
        ++p;
      }
      if (p >= end)
        break;
      type += rbsp[p++];

      uint32_t payload = 0;
      while (p < end && rbsp[p] == 0xFF)
      {
        payload += 255;
        ++p;
      }
      if (p >= end)
        break;
      payload += rbsp[p++];
      if (payload > end - p)
        break;

      if (type == SEI_PAYLOAD_REGISTERED_ITU_T_T35)
        AMLLatchHdr10PlusT35(rbsp.data() + p, payload, meta);
      else if (type == SEI_PAYLOAD_MASTERING_DISPLAY_COLOUR_VOLUME && payload >= 24)
        meta.hdrMdcv = Base64::Encode(reinterpret_cast<const char*>(rbsp.data() + p), payload);
      else if (type == SEI_PAYLOAD_CONTENT_LIGHT_LEVEL_INFO && payload >= 4)
        meta.hdrCll = Base64::Encode(reinterpret_cast<const char*>(rbsp.data() + p), payload);
      p += payload;
    }
  };

  if (nalLengthSize >= 1 && nalLengthSize <= 4)
  {
    size_t pos = 0;
    while (pos + nalLengthSize <= size)
    {
      uint32_t len = 0;
      for (int i = 0; i < nalLengthSize; ++i)
        len = (len << 8) | data[pos + i];
      pos += nalLengthSize;
      if (len == 0 || len > size - pos)
        break;
      parseSeiNal(data + pos, len);
      pos += len;
    }
  }
  else
  {
    size_t nal = SIZE_MAX;
    for (size_t i = 0; i + 2 < size; ++i)
    {
      if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
        continue;
      if (nal != SIZE_MAX)
      {
        size_t end = i;
        while (end > nal && data[end - 1] == 0)
          end--;
        parseSeiNal(data + nal, end - nal);
      }
      nal = i + 3;
      i += 2;
    }
    if (nal != SIZE_MAX && nal < size)
      parseSeiNal(data + nal, size - nal);
  }
}

inline bool AMLReadLeb128(const uint8_t* data, size_t end, size_t& pos, uint64_t& value)
{
  value = 0;
  for (int i = 0; i < 8; ++i)
  {
    if (pos >= end)
      return false;
    const uint8_t b = data[pos++];
    value |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
    if (!(b & 0x80))
      return true;
  }
  return false;
}

// The T.35 payload runs to the last non-zero byte of the OBU, which drops the
// trailing-bits marker and zero padding the same way ffmpeg's
// cbs_av1_get_payload_bytes_left does before it parses HDR10+.
inline size_t AMLAv1T35PayloadLength(const uint8_t* data, size_t size)
{
  size_t len = 0;
  for (size_t i = 0; i < size; ++i)
    if (data[i])
      len = i;
  return len;
}

// Scans AV1 OBUs for the T.35 metadata OBUs and latches the Dolby Vision RPU
// (provider 0x003B) and the HDR10+ payload (provider 0x003C), each from the
// country code on, so an add-on can tell the RPU from an HEVC one by its first
// byte. The RPU keeps the bytes to the end of the OBU, the layout libdovi
// consumes, while HDR10+ is trimmed like ffmpeg trims it.
inline void AMLLatchAv1Metadata(const uint8_t* data, size_t size, AMLFrameMetadata& meta)
{
  if (!data)
    return;

  size_t pos = 0;
  while (pos < size)
  {
    const uint8_t hdr = data[pos];
    if (hdr & 0x80)
      return;
    const int type = (hdr >> 3) & 0x0f;
    const bool extension = hdr & 0x04;
    const bool hasSize = hdr & 0x02;
    pos++;
    if (extension)
      pos++;
    if (pos >= size)
      return;
    uint64_t obuSize = 0;
    if (hasSize)
    {
      if (!AMLReadLeb128(data, size, pos, obuSize))
        return;
    }
    else
      obuSize = size - pos;
    if (obuSize > size - pos)
      return;

    if (type == OBU_METADATA)
    {
      const size_t obuEnd = pos + obuSize;
      size_t p = pos;
      uint64_t metadataType = 0;
      if (AMLReadLeb128(data, obuEnd, p, metadataType) &&
          metadataType == OBU_METADATA_TYPE_ITUT_T35)
      {
        const size_t len = obuEnd - p;
        if (len >= 34 && data[p] == 0xb5 && data[p + 1] == 0x00 && data[p + 2] == 0x3b &&
            data[p + 3] == 0x00 && data[p + 4] == 0x00 && data[p + 5] == 0x08 &&
            data[p + 6] == 0x00)
        {
          meta.doviRpu = Base64::Encode(reinterpret_cast<const char*>(data + p),
                                        static_cast<unsigned int>(len));
        }
        else
          AMLLatchHdr10PlusT35(data + p, AMLAv1T35PayloadLength(data + p, len), meta);
      }
    }
    pos += obuSize;
  }
}

// dvcC/dvvC box layout per ffmpeg's ff_isom_put_dvcc_dvvc, so add-ons read one
// config layout no matter the container
inline std::string AMLSerializeDoviConfig(const AVDOVIDecoderConfigurationRecord& dovi)
{
  uint8_t out[24]{};
  out[0] = dovi.dv_version_major;
  out[1] = dovi.dv_version_minor;
  out[2] = ((dovi.dv_profile & 0x7f) << 1) | ((dovi.dv_level >> 5) & 0x01);
  out[3] = ((dovi.dv_level & 0x1f) << 3) | (!!dovi.rpu_present_flag << 2) |
           (!!dovi.el_present_flag << 1) | !!dovi.bl_present_flag;
  out[4] = ((dovi.dv_bl_signal_compatibility_id & 0x0f) << 4) |
           ((dovi.dv_md_compression & 0x03) << 2);
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}

// MDCV SEI layout: primaries in green, blue, red order in 1/50000 units,
// luminance in 1/10000 nit units; ffmpeg structs store primaries red first
inline std::string AMLSerializeMastering(const AVMasteringDisplayMetadata& mastering)
{
  uint8_t out[24]{};
  size_t p = 0;
  const auto put16 = [&out, &p](long v)
  {
    out[p++] = (v >> 8) & 0xff;
    out[p++] = v & 0xff;
  };
  if (mastering.has_primaries)
  {
    static constexpr int seiOrder[3] = {1, 2, 0};
    for (const int c : seiOrder)
    {
      put16(std::lround(av_q2d(mastering.display_primaries[c][0]) * 50000.0));
      put16(std::lround(av_q2d(mastering.display_primaries[c][1]) * 50000.0));
    }
    put16(std::lround(av_q2d(mastering.white_point[0]) * 50000.0));
    put16(std::lround(av_q2d(mastering.white_point[1]) * 50000.0));
  }
  else
    p = 16;
  if (mastering.has_luminance)
  {
    const auto put32 = [&out, &p](long v)
    {
      out[p++] = (v >> 24) & 0xff;
      out[p++] = (v >> 16) & 0xff;
      out[p++] = (v >> 8) & 0xff;
      out[p++] = v & 0xff;
    };
    put32(std::lround(av_q2d(mastering.max_luminance) * 10000.0));
    put32(std::lround(av_q2d(mastering.min_luminance) * 10000.0));
  }
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}

// CLL SEI layout: max then frame average light level, 16 bits each
inline std::string AMLSerializeContentLight(const AVContentLightMetadata& light)
{
  const uint8_t out[4] = {
      static_cast<uint8_t>(light.MaxCLL >> 8), static_cast<uint8_t>(light.MaxCLL & 0xff),
      static_cast<uint8_t>(light.MaxFALL >> 8), static_cast<uint8_t>(light.MaxFALL & 0xff)};
  return Base64::Encode(reinterpret_cast<const char*>(out), sizeof(out));
}
