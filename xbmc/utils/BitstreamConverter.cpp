/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BitstreamConverter.h"

#include "HevcSei.h"
#include "utils/BitReader.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

enum
{
  AVC_NAL_SLICE = 1,
  AVC_NAL_DPA,
  AVC_NAL_DPB,
  AVC_NAL_DPC,
  AVC_NAL_IDR_SLICE,
  AVC_NAL_SEI,
  AVC_NAL_SPS,
  AVC_NAL_PPS,
  AVC_NAL_AUD,
  AVC_NAL_END_SEQUENCE,
  AVC_NAL_END_STREAM,
  AVC_NAL_FILLER_DATA,
  AVC_NAL_SPS_EXT,
  AVC_NAL_AUXILIARY_SLICE = 19
};

enum
{
  HEVC_NAL_TRAIL_N = 0,
  HEVC_NAL_TRAIL_R = 1,
  HEVC_NAL_TSA_N = 2,
  HEVC_NAL_TSA_R = 3,
  HEVC_NAL_STSA_N = 4,
  HEVC_NAL_STSA_R = 5,
  HEVC_NAL_RADL_N = 6,
  HEVC_NAL_RADL_R = 7,
  HEVC_NAL_RASL_N = 8,
  HEVC_NAL_RASL_R = 9,
  HEVC_NAL_BLA_W_LP = 16,
  HEVC_NAL_BLA_W_RADL = 17,
  HEVC_NAL_BLA_N_LP = 18,
  HEVC_NAL_IDR_W_RADL = 19,
  HEVC_NAL_IDR_N_LP = 20,
  HEVC_NAL_CRA_NUT = 21,
  HEVC_NAL_VPS = 32,
  HEVC_NAL_SPS = 33,
  HEVC_NAL_PPS = 34,
  HEVC_NAL_AUD = 35,
  HEVC_NAL_EOS_NUT = 36,
  HEVC_NAL_EOB_NUT = 37,
  HEVC_NAL_FD_NUT = 38,
  HEVC_NAL_SEI_PREFIX = 39,
  HEVC_NAL_SEI_SUFFIX = 40,
  HEVC_NAL_UNSPEC62 = 62, // Dolby Vision RPU
  HEVC_NAL_UNSPEC63 = 63 // Dolby Vision EL
};

enum
{
  VVC_IDR_W_RADL = 7,
  VVC_IDR_N_LP = 8,
  VVC_CRA_NUT = 9,
  VVC_SPS_NUT = 15
};

enum
{
  SEI_BUFFERING_PERIOD = 0,
  SEI_PIC_TIMING,
  SEI_PAN_SCAN_RECT,
  SEI_FILLER_PAYLOAD,
  SEI_USER_DATA_REGISTERED_ITU_T_T35,
  SEI_USER_DATA_UNREGISTERED,
  SEI_RECOVERY_POINT,
  SEI_DEC_REF_PIC_MARKING_REPETITION,
  SEI_SPARE_PIC,
  SEI_SCENE_INFO,
  SEI_SUB_SEQ_INFO,
  SEI_SUB_SEQ_LAYER_CHARACTERISTICS,
  SEI_SUB_SEQ_CHARACTERISTICS,
  SEI_FULL_FRAME_FREEZE,
  SEI_FULL_FRAME_FREEZE_RELEASE,
  SEI_FULL_FRAME_SNAPSHOT,
  SEI_PROGRESSIVE_REFINEMENT_SEGMENT_START,
  SEI_PROGRESSIVE_REFINEMENT_SEGMENT_END,
  SEI_MOTION_CONSTRAINED_SLICE_GROUP_SET,
  SEI_FILM_GRAIN_CHARACTERISTICS,
  SEI_DEBLOCKING_FILTER_DISPLAY_PREFERENCE,
  SEI_STEREO_VIDEO_INFO,
  SEI_POST_FILTER_HINTS,
  SEI_TONE_MAPPING
};

static const uint8_t* avc_find_startcode_internal(const uint8_t* p, const uint8_t* end)
{
  const uint8_t* a = p + 4 - ((intptr_t)p & 3);

  for (end -= 3; p < a && p < end; p++)
  {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
      return p;
  }

  for (end -= 3; p < end; p += 4)
  {
    uint32_t x = *(const uint32_t*)p;
    if ((x - 0x01010101) & (~x) & 0x80808080) // generic
    {
      if (p[1] == 0)
      {
        if (p[0] == 0 && p[2] == 1)
          return p;
        if (p[2] == 0 && p[3] == 1)
          return p + 1;
      }
      if (p[3] == 0)
      {
        if (p[2] == 0 && p[4] == 1)
          return p + 2;
        if (p[4] == 0 && p[5] == 1)
          return p + 3;
      }
    }
  }

  for (end += 3; p < end; p++)
  {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
      return p;
  }

  return end + 3;
}

static const uint8_t* avc_find_startcode(const uint8_t* p, const uint8_t* end)
{
  const uint8_t* out = avc_find_startcode_internal(p, end);
  if (p < out && out < end && !out[-1])
    out--;
  return out;
}

static FFmpegExtraData hevc_parameter_sets(const uint8_t* data, int size)
{
  const uint8_t* const end = data + size;
  const uint8_t* start = avc_find_startcode(data, end);
  std::vector<uint8_t> parameterSets;
  while (start < end)
  {
    const uint8_t* nal = start;
    while (nal < end && !*nal)
      ++nal;
    if (nal >= end || *nal++ != 1 || nal >= end)
      break;

    const uint8_t* next = avc_find_startcode(nal, end);
    const uint8_t type = (*nal >> 1) & 0x3f;
    if (type == HEVC_NAL_VPS || type == HEVC_NAL_SPS || type == HEVC_NAL_PPS)
      parameterSets.insert(parameterSets.end(), start, next);
    start = next;
  }
  return parameterSets.empty() ? FFmpegExtraData{}
                               : FFmpegExtraData{parameterSets.data(), parameterSets.size()};
}

static bool has_sei_recovery_point(const uint8_t* p, const uint8_t* end, int headerSize = 1)
{
  if (end - p <= headerSize)
    return false;
  p += headerSize;
  while (p < end)
  {
    size_t pt = 0;
    uint8_t byte;
    do
    {
      if (p == end)
        return false;
      byte = *p++;
      pt += byte;
    } while (byte == 0xFF);

    size_t ps = 0;
    do
    {
      if (p == end)
        return false;
      byte = *p++;
      ps += byte;
    } while (byte == 0xFF);

    if (ps > static_cast<size_t>(end - p))
      return false;

    if (pt == SEI_RECOVERY_POINT)
      return ps != 0;
    p += ps;
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
bool CBitstreamParser::CanStartDecode(const uint8_t* buf, int buf_size)
{
  if (!buf)
    return false;

  const uint8_t* const end = buf + buf_size;
  const uint8_t* nal = avc_find_startcode(buf, end);
  while (nal < end)
  {
    while (nal < end && !*nal)
      ++nal;
    if (nal >= end || *nal++ != 1 || nal >= end)
      break;

    const uint8_t* next = avc_find_startcode(nal, end);
    switch (*nal & 0x1f)
    {
      case AVC_NAL_IDR_SLICE:
      case AVC_NAL_SPS:
        return true;
      case AVC_NAL_SEI:
        if (has_sei_recovery_point(nal, next))
          return true;
        break;
      default:
        break;
    }
    nal = next;
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
CBitstreamConverter::CBitstreamConverter()
{
  m_convert_bitstream = false;
  m_convertBuffer = NULL;
  m_convertSize = 0;
  m_start_decode = true;
  m_convert_dovi = false;
  m_removeDovi = false;
  m_removeHdr10Plus = false;
  m_setDoviZeroLevel5 = false;
  m_dovi_el_type = ELType::TYPE_NONE;
}

CBitstreamConverter::~CBitstreamConverter()
{
  Close();
}

bool CBitstreamConverter::Open(enum AVCodecID codec, const uint8_t* in_extradata, int in_extrasize)
{
  m_codec = codec;
  switch (m_codec)
  {
    case AV_CODEC_ID_H264:
      if (in_extrasize < 7 || in_extradata == NULL)
      {
        CLog::Log(LOGERROR, "CBitstreamConverter::Open avcC data too small or missing");
        return false;
      }
      if (in_extradata[0] == 1)
      {
        CLog::LogF(LOGINFO, "Bitstream to annexb init");
        if (!BitstreamConvertInit("h264_mp4toannexb", in_extradata, in_extrasize))
          return false;
        AppendMvcParameterSets(in_extradata, in_extrasize);
        return true;
      }
      CLog::LogF(LOGINFO, "Invalid avcC");
      return false;
    case AV_CODEC_ID_HEVC:
      if (in_extrasize < 23 || in_extradata == NULL)
      {
        CLog::Log(LOGERROR, "CBitstreamConverter::Open hvcC data too small or missing");
        return false;
      }
      /**
       * It seems the extradata is encoded as hvcC format.
       * Temporarily, we support configurationVersion==0 until 14496-15 3rd
       * is finalized. When finalized, configurationVersion will be 1 and we
       * can recognize hvcC by checking if extradata[0]==1 or not.
       */
      if (in_extradata[0] || in_extradata[1] || in_extradata[2] > 1)
      {
        CLog::LogF(LOGINFO, "Bitstream to annexb init");
        return BitstreamConvertInit("hevc_mp4toannexb", in_extradata, in_extrasize);
      }
      CLog::LogF(LOGINFO, "Invalid hvcC");
      return false;
    case AV_CODEC_ID_VVC:
      // vvcC starts with five reserved bits set to one, Annex B starts with a start code
      if (in_extradata && in_extrasize > 1 && (in_extradata[0] & 0xf8) == 0xf8)
      {
        CLog::LogF(LOGINFO, "VVC, convert to annexb format");
        BitstreamConvertInit("vvc_mp4toannexb", in_extradata, in_extrasize);
      }
      else
        CLog::LogF(LOGINFO, "VVC, no extra data");
      return true;
    default:
      return false;
  }
}

bool CBitstreamConverter::Open(enum AVCodecID codec,
                               const uint8_t* in_extradata,
                               int in_extrasize,
                               bool to_annexb)
{
  m_to_annexb = to_annexb;
  return Open(codec, in_extradata, in_extrasize);
}

void CBitstreamConverter::Close()
{
  av_bsf_free(&m_bitstreamFilter);

  if (m_convertBuffer)
    av_free(m_convertBuffer), m_convertBuffer = NULL;
  m_convertSize = 0;

  m_inputBuffer = nullptr;
  m_inputSize = 0;

  m_extraData = {};

  m_convert_bitstream = false;
}

bool CBitstreamConverter::Convert(uint8_t* pData, int iSize)
{
  if (m_convertBuffer)
  {
    av_free(m_convertBuffer);
    m_convertBuffer = NULL;
  }
  m_convertSize = 0;
  m_inputBuffer = nullptr;
  m_inputSize = 0;

  if (!pData || iSize <= 0)
    return false;

  if (m_codec == AV_CODEC_ID_VVC && !m_vvcFormatChecked)
  {
    m_vvcAnnexB = !m_convert_bitstream || IsAnnexB(pData, iSize);
    m_vvcFormatChecked = true;
  }
  if (m_codec == AV_CODEC_ID_VVC && m_vvcAnnexB)
  {
    if (!m_start_decode)
      m_start_decode = HasVvcStartAnnexB(pData, iSize);
    m_inputBuffer = pData;
    m_inputSize = iSize;
    return true;
  }

  // Annex B input is passed through as is
  if (m_to_annexb && !m_convert_bitstream &&
      (m_codec == AV_CODEC_ID_H264 || m_codec == AV_CODEC_ID_HEVC))
  {
    m_inputBuffer = pData;
    m_inputSize = iSize;
    return true;
  }

  return m_convert_bitstream && BitstreamConvert(pData, iSize);
}

bool CBitstreamConverter::Convert(uint8_t* pData_bl,
                                  int iSize_bl,
                                  uint8_t* pData_el,
                                  int iSize_el)
{
  if (m_convertBuffer)
  {
    av_free(m_convertBuffer);
    m_convertBuffer = NULL;
  }
  m_convertSize = 0;
  m_inputBuffer = nullptr;
  m_inputSize = 0;

  if (!pData_bl || !pData_el)
    return true;

  const auto forEachNal = [this](const uint8_t* data, int size, const auto& func)
  {
    const uint8_t* const end = data + size;
    if (m_convert_bitstream)
    {
      while (end - data > m_nalLengthSize)
      {
        uint32_t nalSize = 0;
        for (uint8_t i = 0; i < m_nalLengthSize; ++i)
          nalSize = (nalSize << 8) | *data++;
        nalSize = std::min<uint32_t>(nalSize, static_cast<uint32_t>(end - data));
        if (nalSize)
          func(data, nalSize);
        data += nalSize;
      }
      return;
    }

    const uint8_t* nal = avc_find_startcode(data, end);
    while (nal < end)
    {
      while (nal < end && !*nal)
        ++nal;
      if (nal >= end || *nal++ != 1 || nal >= end)
        break;
      const uint8_t* next = avc_find_startcode(nal, end);
      func(nal, static_cast<uint32_t>(next - nal));
      nal = next;
    }
  };

  std::vector<uint8_t> output;
  const auto append = [&output](const uint8_t* nal, uint32_t size,
                                std::initializer_list<uint8_t> startCode)
  {
    output.insert(output.end(), startCode);
    output.insert(output.end(), nal, nal + size);
  };

  forEachNal(pData_bl, iSize_bl,
             [&](const uint8_t* nal, uint32_t size)
             {
               if (output.empty())
                 append(nal, size, {0, 0, 0, 1});
               else
                 append(nal, size, {0, 0, 1});
             });

  // the enhancement layer goes into the base layer package as NAL type 63
  forEachNal(pData_el, iSize_el,
             [&](const uint8_t* nal, uint32_t size)
             {
               if (((*nal >> 1) & 0x3f) == HEVC_NAL_UNSPEC62)
               {
#ifdef HAVE_LIBDOVI
                 const DoviData* rpuData = processDoviRpu(const_cast<uint8_t*>(nal), size);
                 if (rpuData)
                 {
                   append(rpuData->data, static_cast<uint32_t>(rpuData->len), {0, 0, 0, 1});
                   dovi_data_free(rpuData);
                   return;
                 }
#endif
                 append(nal, size, {0, 0, 0, 1});
               }
               else if (!m_convert_dovi)
                 append(nal, size, {0, 0, 1, HEVC_NAL_UNSPEC63 << 1, 1});
             });

  if (output.empty())
    return true;

  m_convertBuffer = static_cast<uint8_t*>(av_memdup(output.data(), output.size()));
  if (!m_convertBuffer)
    return false;

  m_convertSize = static_cast<int>(output.size());
  return true;
}

uint8_t* CBitstreamConverter::GetConvertBuffer() const
{
  return m_inputBuffer ? m_inputBuffer : m_convertBuffer;
}

int CBitstreamConverter::GetConvertSize() const
{
  return m_inputBuffer ? m_inputSize : m_convertSize;
}

uint8_t* CBitstreamConverter::GetExtraData()
{
  return m_extraData.GetData();
}
const uint8_t* CBitstreamConverter::GetExtraData() const
{
  return m_extraData.GetData();
}
int CBitstreamConverter::GetExtraSize() const
{
  return m_extraData.GetSize();
}

void CBitstreamConverter::ResetStartDecode()
{
  m_start_decode = false;
  // Forget the parameter sets seen before, so the next IDR picture gets them again
  if (m_bitstreamFilter)
    av_bsf_flush(m_bitstreamFilter);
}

bool CBitstreamConverter::CanStartDecode() const
{
  return m_start_decode;
}

bool CBitstreamConverter::BitstreamConvertInit(const char* filterName,
                                               const uint8_t* extraData,
                                               int extraDataSize)
{
  const AVBitStreamFilter* filter{av_bsf_get_by_name(filterName)};
  if (!filter)
  {
    CLog::LogF(LOGERROR, "Bitstream filter {} not found", filterName);
    return false;
  }

  int result{av_bsf_alloc(filter, &m_bitstreamFilter)};
  if (result < 0)
  {
    CLog::LogF(LOGERROR, "Failed to allocate {}: {}", filterName,
               FFMPEG_HELP_TOOLS::FFMpegErrorToString(result));
    return false;
  }

  AVCodecParameters* codecParameters{m_bitstreamFilter->par_in};
  codecParameters->codec_type = AVMEDIA_TYPE_VIDEO;
  codecParameters->codec_id = m_codec;
  codecParameters->extradata = static_cast<uint8_t*>(
      av_mallocz(static_cast<size_t>(extraDataSize) + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!codecParameters->extradata)
  {
    av_bsf_free(&m_bitstreamFilter);
    return false;
  }

  std::copy_n(extraData, extraDataSize, codecParameters->extradata);
  codecParameters->extradata_size = extraDataSize;
  m_bitstreamFilter->time_base_in = {1, AV_TIME_BASE};

  result = av_bsf_init(m_bitstreamFilter);
  if (result < 0)
  {
    CLog::LogF(LOGERROR, "Failed to initialize {}: {}", filterName,
               FFMPEG_HELP_TOOLS::FFMpegErrorToString(result));
    av_bsf_free(&m_bitstreamFilter);
    return false;
  }

  if (m_bitstreamFilter->par_out->extradata && m_bitstreamFilter->par_out->extradata_size > 0)
  {
    const AVCodecParameters* output = m_bitstreamFilter->par_out;
    // Decoder configuration contains parameter sets; SEI filtering happens on packets.
    m_extraData =
        m_codec == AV_CODEC_ID_HEVC
            ? hevc_parameter_sets(output->extradata, output->extradata_size)
            : FFmpegExtraData{output->extradata, static_cast<size_t>(output->extradata_size)};
  }

  if (m_codec == AV_CODEC_ID_VVC)
    m_nalLengthSize = ((extraData[0] >> 1) & 3) + 1;
  else
    m_nalLengthSize = (extraData[m_codec == AV_CODEC_ID_H264 ? 4 : 21] & 3) + 1;
  m_convert_bitstream = true;
  return true;
}

void CBitstreamConverter::AppendMvcParameterSets(const uint8_t* extraData, int extraDataSize)
{
  // the mvcC atom after the avcC carries the SPS and PPS of the dependent view
  const uint8_t* const end = extraData + extraDataSize;
  const uint8_t* p = std::search(extraData, end, "mvcC", "mvcC" + 4);
  if (p == end || end - p < 4 + 6)
    return;
  p += 4 + 5;

  std::vector<uint8_t> parameterSets(m_extraData.GetData(),
                                     m_extraData.GetData() + m_extraData.GetSize());
  for (int list = 0; list < 2 && p < end; ++list)
  {
    int count = list == 0 ? (*p++ & 0x1f) : *p++;
    while (count-- > 0 && end - p >= 2)
    {
      const uint16_t size = static_cast<uint16_t>(p[0] << 8 | p[1]);
      p += 2;
      if (size > end - p)
        return;
      parameterSets.insert(parameterSets.end(), {0, 0, 0, 1});
      parameterSets.insert(parameterSets.end(), p, p + size);
      p += size;
    }
  }
  m_extraData = FFmpegExtraData{parameterSets.data(), parameterSets.size()};
}

bool CBitstreamConverter::IsIDR(uint8_t unit_type)
{
  switch (m_codec)
  {
    case AV_CODEC_ID_H264:
      return unit_type == AVC_NAL_IDR_SLICE;
    case AV_CODEC_ID_HEVC:
      return unit_type == HEVC_NAL_IDR_W_RADL || unit_type == HEVC_NAL_IDR_N_LP ||
             unit_type == HEVC_NAL_CRA_NUT;
    case AV_CODEC_ID_VVC:
      return unit_type == VVC_IDR_W_RADL || unit_type == VVC_IDR_N_LP || unit_type == VVC_CRA_NUT;
    default:
      return false;
  }
}

bool CBitstreamConverter::IsAnnexB(const uint8_t* data, int size)
{
  return size >= 3 && !data[0] && !data[1] &&
         (data[2] == 1 || (size >= 4 && !data[2] && data[3] == 1));
}

bool CBitstreamConverter::HasVvcStartAnnexB(const uint8_t* data, int size)
{
  const uint8_t* const end = data + size;
  const uint8_t* nal = avc_find_startcode(data, end);
  while (nal < end)
  {
    while (nal < end && !*nal)
      ++nal;
    if (nal >= end || *nal++ != 1 || nal + 1 >= end)
      break;

    const uint8_t unitType = (nal[1] >> 3) & 0x1f;
    if (unitType == VVC_SPS_NUT || IsIDR(unitType))
      return true;
    nal = avc_find_startcode(nal, end);
  }
  return false;
}

bool CBitstreamConverter::HasRecoveryPoint(const uint8_t* data, int size)
{
  const uint8_t* const end = data + size;
  const bool h264 = m_codec == AV_CODEC_ID_H264;
  const bool vvc = m_codec == AV_CODEC_ID_VVC;
  const uint8_t nalSps = h264  ? static_cast<uint8_t>(AVC_NAL_SPS)
                         : vvc ? static_cast<uint8_t>(VVC_SPS_NUT)
                               : static_cast<uint8_t>(HEVC_NAL_SPS);
  const uint8_t nalSei =
      h264 ? static_cast<uint8_t>(AVC_NAL_SEI) : static_cast<uint8_t>(HEVC_NAL_SEI_PREFIX);
  while (data < end)
  {
    if (end - data < m_nalLengthSize)
      return false;

    uint32_t nalSize = 0;
    for (uint8_t i = 0; i < m_nalLengthSize; ++i)
      nalSize = (nalSize << 8) | *data++;
    if (nalSize > static_cast<size_t>(end - data))
      return false;
    if (nalSize == 0)
      continue;

    if (vvc && nalSize < 2)
      return false;
    const uint8_t unitType =
        h264 ? (*data & 0x1f) : vvc ? ((data[1] >> 3) & 0x1f) : ((*data >> 1) & 0x3f);
    if (unitType == nalSps || IsIDR(unitType) ||
        (unitType == nalSei && has_sei_recovery_point(data, data + nalSize, h264 ? 1 : 2)))
      return true;
    data += nalSize;
  }
  return false;
}

bool CBitstreamConverter::BitstreamConvert(const uint8_t* pData, int iSize)
{
  const auto freePacket = [](AVPacket* packetToFree) { av_packet_free(&packetToFree); };
  std::unique_ptr<AVPacket, decltype(freePacket)> packet{av_packet_alloc(), freePacket};
  if (!packet)
    return false;

  int result{av_new_packet(packet.get(), iSize)};
  if (result >= 0)
  {
    std::copy_n(pData, iSize, packet->data);
    result = av_bsf_send_packet(m_bitstreamFilter, packet.get());
  }
  if (result >= 0)
    result = av_bsf_receive_packet(m_bitstreamFilter, packet.get());

  if (result < 0)
  {
    CLog::LogF(LOGERROR, "Error converting: {}", FFMPEG_HELP_TOOLS::FFMpegErrorToString(result));
    return false;
  }

  if (!ProcessAnnexB(packet->data, packet->size))
    return false;

  // Parameter sets injected by FFmpeg do not signal recovery in the input stream.
  if (!m_start_decode)
    m_start_decode = HasRecoveryPoint(pData, iSize);
  return true;
}

bool CBitstreamConverter::ProcessAnnexB(uint8_t* data, int size)
{
  const uint8_t* const bufferEnd{data + size};
  const uint8_t* nalStart{avc_find_startcode(data, bufferEnd)};

#ifdef HAVE_LIBDOVI
  const DoviData* rpuData{nullptr};
#endif

  std::vector<uint8_t> finalPrefixSeiNalu;
  std::vector<uint8_t> output;
  const bool rewrite{m_removeDovi || m_removeHdr10Plus || m_convert_dovi || m_setDoviZeroLevel5};
  while (nalStart < bufferEnd)
  {
    while (nalStart < bufferEnd && !*nalStart)
      ++nalStart;
    if (nalStart >= bufferEnd || *nalStart++ != 1 || nalStart >= bufferEnd)
      break;

    const uint8_t* nalEnd{avc_find_startcode(nalStart, bufferEnd)};
    const int nalSize{static_cast<int>(nalEnd - nalStart)};
    const uint8_t unitType{m_codec == AV_CODEC_ID_H264
                               ? static_cast<uint8_t>(*nalStart & 0x1f)
                               : static_cast<uint8_t>((*nalStart >> 1) & 0x3f)};

    bool writeBuffer{true};
    const uint8_t* bufferToWrite{nalStart};
    int32_t finalNalSize{nalSize};
    bool containsHdr10Plus{false};

    if (m_removeDovi && (unitType == HEVC_NAL_UNSPEC62 || unitType == HEVC_NAL_UNSPEC63))
      writeBuffer = false;

    // Try removing HDR10+ only if the NAL is big enough, optimization
    if (m_removeHdr10Plus && unitType == HEVC_NAL_SEI_PREFIX && nalSize >= 7)
    {
      std::tie(containsHdr10Plus, finalPrefixSeiNalu) =
          CHevcSei::RemoveHdr10PlusFromSeiNalu(nalStart, nalSize);

      if (containsHdr10Plus)
      {
        if (!finalPrefixSeiNalu.empty())
        {
          bufferToWrite = finalPrefixSeiNalu.data();
          finalNalSize = static_cast<int32_t>(finalPrefixSeiNalu.size());
        }
        else
        {
          writeBuffer = false;
        }
      }
    }

    if (writeBuffer)
    {
      if (unitType == HEVC_NAL_UNSPEC62)
      {
#ifdef HAVE_LIBDOVI
        // Convert the RPU itself
        rpuData = processDoviRpu(const_cast<uint8_t*>(nalStart), nalSize);
        if (rpuData)
        {
          bufferToWrite = rpuData->data;
          finalNalSize = static_cast<int32_t>(rpuData->len);
        }
#endif
      }
      else if (m_convert_dovi && unitType == HEVC_NAL_UNSPEC63)
      {
        // Ignore the enhancement layer, may or may not help
        writeBuffer = false;
      }
    }

    if (rewrite && writeBuffer)
    {
      const size_t startCodeSize{output.empty() || unitType == HEVC_NAL_UNSPEC62 ? 4U : 3U};
      if (output.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - startCodeSize ||
          static_cast<size_t>(finalNalSize) >
              static_cast<size_t>(std::numeric_limits<int>::max()) - output.size() - startCodeSize)
      {
#ifdef HAVE_LIBDOVI
        if (rpuData)
          dovi_data_free(rpuData);
#endif
        return false;
      }

      if (startCodeSize == 4)
        output.push_back(0);
      output.insert(output.end(), {0, 0, 1});
      output.insert(output.end(), bufferToWrite, bufferToWrite + finalNalSize);
    }

#ifdef HAVE_LIBDOVI
    if (rpuData)
    {
      dovi_data_free(rpuData);
      rpuData = nullptr;
    }
#endif

    if (containsHdr10Plus && !finalPrefixSeiNalu.empty())
      finalPrefixSeiNalu.clear();

    nalStart = nalEnd;
  }

  const uint8_t* outputData{rewrite ? output.data() : data};
  const size_t outputSize{rewrite ? output.size() : static_cast<size_t>(size)};
  if (outputSize == 0)
    return false;

  m_convertBuffer = static_cast<uint8_t*>(av_memdup(outputData, outputSize));
  if (!m_convertBuffer)
    return false;

  m_convertSize = static_cast<int>(outputSize);
  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
bool CBitstreamConverter::mpeg2_sequence_header(const uint8_t* data,
                                                const uint32_t size,
                                                mpeg2_sequence* sequence)
{
  // parse nal's until a sequence_header_code is found
  // and return the width, height, aspect ratio and frame rate if changed.
  bool changed = false;

  if (!data)
    return changed;

  const uint8_t* p = data;
  const uint8_t* end = p + size;
  const uint8_t *nal_start, *nal_end;

  nal_start = avc_find_startcode(p, end);
  while (nal_start < end)
  {
    while (!*(nal_start++))
      ;
    nal_end = avc_find_startcode(nal_start, end);
    if (*nal_start == 0xB3)
    {
      CBitReader bs({nal_start, static_cast<size_t>(nal_end - nal_start)});

      // The code and the four fields read below take 40 bits; a header cut short changes nothing
      if (bs.BitsLeft() < 40)
      {
        nal_start = nal_end;
        continue;
      }

      // sequence_header_code
      bs.ReadBits(8);

      // width
      // nal_start + 12 bits == horizontal_size_value
      uint32_t width = bs.ReadBits(12);
      if (width != sequence->width)
      {
        changed = true;
        sequence->width = width;
      }
      // height
      // nal_start + 24 bits == vertical_size_value
      uint32_t height = bs.ReadBits(12);
      if (height != sequence->height)
      {
        changed = true;
        sequence->height = height;
      }

      // aspect ratio
      // nal_start + 28 bits == aspect_ratio_information
      float ratio = sequence->ratio;
      uint32_t ratio_info = bs.ReadBits(4);
      switch (ratio_info)
      {
        case 0x01:
          ratio = 1.0f;
          break;
        default:
        case 0x02:
          ratio = 4.0f / 3;
          break;
        case 0x03:
          ratio = 16.0f / 9;
          break;
        case 0x04:
          ratio = 2.21f;
          break;
      }
      if (ratio_info != sequence->ratio_info)
      {
        changed = true;
        sequence->ratio = ratio;
        sequence->ratio_info = ratio_info;
      }

      // frame rate
      // nal_start + 32 bits == frame_rate_code
      uint32_t fpsrate = sequence->fps_rate;
      uint32_t fpsscale = sequence->fps_scale;
      uint32_t rate_info = bs.ReadBits(4);

      switch (rate_info)
      {
        default:
        case 0x01:
          fpsrate = 24000;
          fpsscale = 1001;
          break;
        case 0x02:
          fpsrate = 24000;
          fpsscale = 1000;
          break;
        case 0x03:
          fpsrate = 25000;
          fpsscale = 1000;
          break;
        case 0x04:
          fpsrate = 30000;
          fpsscale = 1001;
          break;
        case 0x05:
          fpsrate = 30000;
          fpsscale = 1000;
          break;
        case 0x06:
          fpsrate = 50000;
          fpsscale = 1000;
          break;
        case 0x07:
          fpsrate = 60000;
          fpsscale = 1001;
          break;
        case 0x08:
          fpsrate = 60000;
          fpsscale = 1000;
          break;
      }

      if (fpsscale != sequence->fps_scale || fpsrate != sequence->fps_rate)
      {
        changed = true;
        sequence->fps_rate = fpsrate;
        sequence->fps_scale = fpsscale;
      }
    }
    nal_start = nal_end;
  }

  return changed;
}

bool CBitstreamConverter::h264_sequence_header(const uint8_t *data, const uint32_t size, h264_sequence *sequence)
{
    // parse nal units until SPS is found
    // and return the width, height and aspect ratio if changed.
    bool changed = false;

    if (!data)
        return changed;

    const uint8_t *p = data;
    const uint8_t *end = p + size;
    const uint8_t *nal_start, *nal_end;

    int profile_idc;
    int chroma_format_idc = 1;
    uint8_t pic_order_cnt_type;
    uint8_t aspect_ratio_idc = 0;
    uint8_t separate_colour_plane_flag = 0;
    int8_t frame_mbs_only_flag = -1;
    unsigned int pic_width, pic_width_cropped;
    unsigned int pic_height, pic_height_cropped;
    unsigned int frame_crop_right_offset = 0;
    unsigned int frame_crop_bottom_offset = 0;
    unsigned int sar_width = 0;
    unsigned int sar_height = 0;

    int lastScale;
    int nextScale;
    int deltaScale;

    nal_start = avc_find_startcode(p, end);

    while (nal_start < end)
    {
        while (!*(nal_start++));

        nal_end = avc_find_startcode(nal_start, end);

        if ((*nal_start & 0x1f) == 7) // SPS
        {
            // drop the emulation prevention bytes, CBitReader reads the plain RBSP
            std::vector<uint8_t> rbsp;
            rbsp.reserve(nal_end - nal_start);
            int zeros = 0;
            for (const uint8_t* q = nal_start; q < nal_end; ++q)
            {
                if (*q == 3 && zeros >= 2)
                {
                    zeros = 0;
                    continue;
                }
                zeros = *q ? 0 : zeros + 1;
                rbsp.push_back(*q);
            }
            CBitReader bs({rbsp.data(), rbsp.size()});

            bs.ReadBits(8); // NAL unit type

            profile_idc = bs.ReadBits(8);  // profile_idc

            bs.ReadBits(1);  // constraint_set0_flag
            bs.ReadBits(1);  // constraint_set1_flag
            bs.ReadBits(1);  // constraint_set2_flag
            bs.ReadBits(1);  // constraint_set3_flag
            bs.ReadBits(4);  // reserved
            bs.ReadBits(8);  // level_idc
            bs.ReadUE();  // sps_id

            if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
                profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
                profile_idc == 86  || profile_idc == 118 || profile_idc == 128 )
            {

                chroma_format_idc = bs.ReadUE(); // chroma_format_idc
                // high_profile
                if (chroma_format_idc == 3)
                {
                    separate_colour_plane_flag = bs.ReadBits(1); // separate_colour_plane_flag
                }

                bs.ReadUE(); // bit_depth_luma_minus8
                bs.ReadUE(); // bit_depth_chroma_minus8
                bs.ReadBits(1); // qpprime_y_zero_transform_bypass_flag

                if (bs.ReadBits(1)) // seq_scaling_matrix_present_flag
                {

                    for (int idx = 0; idx < ((chroma_format_idc != 3) ? 8 : 12); ++idx)
                    {
                        if (bs.ReadBits(1)) // scaling list present
                        {
                            lastScale = nextScale = 8;
                            int sl_n = ((idx < 6) ? 16 : 64);

                            for(int sl_i = 0; sl_i < sl_n; ++sl_i)
                            {
                                if (nextScale != 0)
                                {
                                    deltaScale = bs.ReadSE();
                                    nextScale = (lastScale + deltaScale + 256) % 256;

                                }
                                lastScale = (nextScale == 0) ? lastScale : nextScale;
                            }
                        }
                    }
                }
            }

            bs.ReadUE(); // log2_max_frame_num_minus4

            pic_order_cnt_type = bs.ReadUE(); // pic_order_cnt_type

            if (pic_order_cnt_type == 0)
                bs.ReadUE(); //  log2_max_pic_order_cnt_lsb_minus4
            else if (pic_order_cnt_type == 1)
            {
                bs.ReadBits(1); // delta_pic_order_always_zero_flag
                bs.ReadSE(); // offset_for_non_ref_pic
                bs.ReadSE(); // offset_for_top_to_bottom_field

                unsigned int tmp, idx;
                tmp =  bs.ReadUE();
                for (idx = 0; idx < tmp; ++idx)
                    bs.ReadSE(); // offset_for_ref_frame[i]
            }

            bs.ReadUE(); // num_ref_frames
            bs.ReadBits(1); // gaps_in_frame_num_allowed_flag

            pic_width = (bs.ReadUE() + 1) * 16 ; // pic_width
            pic_height = (bs.ReadUE() + 1) * 16; // pic_height

            frame_mbs_only_flag = bs.ReadBits(1); // frame_mbs_only_flag
            if (!frame_mbs_only_flag)
            {
                pic_height *= 2;
                bs.ReadBits(1); // mb_adaptive_frame_field_flag
            }

            bs.ReadBits(1); // direct_8x8_inference_flag

            if (bs.ReadBits(1)) // frame_cropping_flag
            {
                bs.ReadUE(); // frame_crop_left_offset
                frame_crop_right_offset = bs.ReadUE(); // frame_crop_right_offset
                bs.ReadUE(); // frame_crop_top_offset
                frame_crop_bottom_offset = bs.ReadUE(); // frame_crop_bottom_offset
            }

            if (bs.ReadBits(1)) // vui_parameters_present_flag
            {
                if (bs.ReadBits(1)) //aspect_ratio_info_present_flag
                {
                    aspect_ratio_idc = bs.ReadBits(8); // aspect_ratio_idc

                    if (aspect_ratio_idc == 255) // EXTENDED_SAR
                    {
                        sar_width  = bs.ReadBits(16);
                        sar_height = bs.ReadBits(16);

                    }
                }

                if (bs.ReadBits(1)) //overscan_info_present_flag
                    bs.ReadBits(1); //overscan_appropriate_flag

                if (bs.ReadBits(1))  //video_signal_type_present_flag
                {
                    bs.ReadBits(3); //video_format
                    bs.ReadBits(1); //video_full_range_flag
                    if (bs.ReadBits(1)) // colour_description_present_flag
                    {
                        bs.ReadBits(8); // colour_primaries
                        bs.ReadBits(8); // transfer_characteristics
                        bs.ReadBits(8); // matrix_coefficients
                    }
                }

                if (bs.ReadBits(1)) //chroma_loc_info_present_flag
                {
                    bs.ReadUE(); //chroma_sample_loc_type_top_field ue(v)
                    bs.ReadUE(); //chroma_sample_loc_type_bottom_field ue(v)
                }

                if (bs.ReadBits(1)) //timing_info_present_flag
                {
                    bs.ReadBits(32); //num_units_in_tick
                    bs.ReadBits(32); //time_scale
                    bs.ReadBits(1); // fixed rate
                }
            }

            unsigned int ChromaArrayType, crop;
            ChromaArrayType = separate_colour_plane_flag ? 0 : chroma_format_idc;

            // cropped width
            unsigned int CropUnitX, SubWidthC;
            CropUnitX = 1;
            SubWidthC = chroma_format_idc == 3 ? 1 : 2;
            if (ChromaArrayType != 0)
                CropUnitX = SubWidthC;
            crop = CropUnitX * frame_crop_right_offset;
            pic_width_cropped = pic_width - crop;

            if (pic_width_cropped != sequence->width)
            {
                changed = true;
                sequence->width = pic_width_cropped;
            }

            // cropped height
            unsigned int CropUnitY, SubHeightC;
            CropUnitY = 2 - frame_mbs_only_flag;
            SubHeightC = chroma_format_idc <= 1 ? 2 : 1;
            if (ChromaArrayType != 0)
                CropUnitY *= SubHeightC;
            crop = CropUnitY * frame_crop_bottom_offset;
            pic_height_cropped = pic_height - crop;

            if (pic_height_cropped != sequence->height)
            {
                changed = true;
                sequence->height = pic_height_cropped;
            }

            // aspect ratio
            float ratio = sequence->ratio;
            if (pic_height_cropped)
                ratio = pic_width_cropped / (double) pic_height_cropped;
            switch (aspect_ratio_idc)
            {
                case 0:
                    // Unspecified
                    break;
                case 1:
                    // 1:1
                    break;
                case 2:
                    // 12:11
                    ratio *= 1.0909090909090908f;
                    break;
                case 3:
                    // 10:11
                    ratio *= 0.90909090909090906f;
                    break;
                case 4:
                    // 16:11
                    ratio *= 1.4222222222222222f;
                    break;
                case 5:
                    // 40:33
                    ratio *= 1.2121212121212122f;
                    break;
                case 6:
                    // 24:11
                    ratio *= 2.1818181818181817f;
                    break;
                case 7:
                    // 20:11
                    ratio *= 1.8181818181818181f;
                    break;
                case 8:
                    // 32:11
                    ratio *= 2.9090909090909092f;
                    break;
                case 9:
                    // 80:33
                    ratio *= 2.4242424242424243f;
                    break;
                case 10:
                    // 18:11
                    ratio *= 1.6363636363636365f;
                    break;
                case 11:
                    // 15:11
                    ratio *= 1.3636363636363635f;
                    break;
                case 12:
                    // 64:33
                    ratio *= 1.9393939393939394f;
                    break;
                case 13:
                    // 160:99
                    ratio *= 1.6161616161616161f;
                    break;
                case 14:
                    // 4:3
                    ratio *= 1.3333333333333333f;
                    break;
                case 15:
                    // 3:2
                    ratio *= 1.5f;
                    break;
                case 16:
                    // 2:1
                    ratio *= 2.0f;
                    break;
                case 255:
                    // EXTENDED_SAR
                    if (sar_height)
                        ratio *= sar_width / (float)sar_height;
                    else
                        ratio = 0.0f;
                    break;
            } // switch
            if (aspect_ratio_idc != sequence->ratio_info)
            {
                changed = true;
                sequence->ratio = ratio;
                sequence->ratio_info = aspect_ratio_idc;
            }
            if (changed)
            {
              CLog::Log(LOGDEBUG, "CBitstreamConverter::h264_sequence_header: "
                "width({:d}), height({:d}), ratio({:f}), {:d}x{:d}", pic_width_cropped, pic_height_cropped, ratio, sar_width, sar_height);
            }

            break;
        } // SPS
        nal_start = nal_end;
    }

    return changed;
}

#ifdef HAVE_LIBDOVI
// Processes Dolby Vision RPU
//   - Updates `m_dovi_el_type` according to the current header
//   - Converts to profile 8.1 if `m_convert_dovi` is enabled
//   - Sets level 5 metadata to 0 offsets if `m_setDoviZeroLevel5` is enabled
//
// The returned data must be freed with `dovi_data_free`
// May be NULL if no processing was done or if parsing errored
const DoviData* CBitstreamConverter::processDoviRpu(uint8_t* buf, uint32_t nalSize)
{
  // early exit if no processing option is enabled
  if (!m_convert_dovi && !m_setDoviZeroLevel5)
    return NULL;

  DoviRpuOpaque* rpu = dovi_parse_unspec62_nalu(buf, nalSize);
  const DoviRpuDataHeader* header = dovi_rpu_get_header(rpu);
  const DoviData* rpuData = NULL;

  int ret = 0;
  bool processed = false;

  if (!header)
  {
    dovi_rpu_free(rpu);
    return rpuData;
  }

  if (m_dovi_el_type == ELType::TYPE_NONE && header->el_type &&
      (header->guessed_profile == 4 || header->guessed_profile == 7))
  {
    if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
      m_dovi_el_type = ELType::TYPE_FEL;
    else if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
      m_dovi_el_type = ELType::TYPE_MEL;
  }

  if (m_convert_dovi && header->guessed_profile == 7)
  {
    ret = dovi_convert_rpu_with_mode(rpu, 2);
    processed = true;
  }

  if (ret == 0 && m_setDoviZeroLevel5)
  {
    ret = dovi_rpu_set_active_area_offsets(rpu, 0, 0, 0, 0);
    processed = true;
  }

  if (ret == 0 && processed)
    rpuData = dovi_write_unspec62_nalu(rpu);

  dovi_rpu_free_header(header);
  dovi_rpu_free(rpu);

  return rpuData;
}
#endif
