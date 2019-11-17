/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDStreamInfo.h"

#include "DVDDemuxers/DVDDemux.h"
#include "cores/VideoPlayer/Interface/Addon/DemuxCrypto.h"

CDVDStreamInfo::CDVDStreamInfo()                                                     { extradata = NULL; Clear(); }
CDVDStreamInfo::CDVDStreamInfo(const CDVDStreamInfo &right, bool withextradata )     { extradata = NULL; Clear(); Assign(right, withextradata); }
CDVDStreamInfo::CDVDStreamInfo(const CDemuxStream &right, bool withextradata )       { extradata = NULL; Clear(); Assign(right, withextradata); }

CDVDStreamInfo::~CDVDStreamInfo()
{
  if( extradata && extrasize ) free(extradata);

  extradata = NULL;
  extrasize = 0;
}


void CDVDStreamInfo::Clear()
{
  codec = AV_CODEC_ID_NONE;
  type = STREAM_NONE;
  uniqueId = -1;
  codecOptions = 0;
  codec_tag  = 0;
  flags = 0;
  filename.clear();
  dvd = false;

  if( extradata && extrasize ) free(extradata);

  extradata = NULL;
  extrasize = 0;

  cryptoSession = nullptr;
  externalInterfaces = nullptr;

  fpsscale = 0;
  fpsrate  = 0;
  height   = 0;
  width    = 0;
  aspect   = 0.0;
  vfr      = false;
  stills   = false;
  level    = 0;
  profile  = 0;
  ptsinvalid = false;
  forced_aspect = false;
  bitsperpixel = 0;
  stereo_mode.clear();

  channels   = 0;
  samplerate = 0;
  blockalign = 0;
  bitrate    = 0;
  bitspersample = 0;
  channellayout = 0;

  orientation = 0;
}

bool CDVDStreamInfo::Equal(const CDVDStreamInfo& right, bool withextradata)
{
  if( codec     != right.codec
  ||  type      != right.type
  ||  uniqueId  != right.uniqueId
  ||  demuxerId != right.demuxerId
  ||  codec_tag != right.codec_tag
  ||  flags     != right.flags)
    return false;

  if( withextradata )
  {
    if( extrasize != right.extrasize ) return false;
    if( extrasize )
    {
      if( memcmp(extradata, right.extradata, extrasize) != 0 ) return false;
    }
  }

  // VIDEO
  if( fpsscale != right.fpsscale
  ||  fpsrate  != right.fpsrate
  ||  height   != right.height
  ||  width    != right.width
  ||  stills   != right.stills
  ||  level    != right.level
  ||  profile  != right.profile
  ||  ptsinvalid != right.ptsinvalid
  ||  forced_aspect != right.forced_aspect
  ||  bitsperpixel != right.bitsperpixel
  ||  vfr      != right.vfr
  ||  stereo_mode != right.stereo_mode ) return false;

  // AUDIO
  if( channels      != right.channels
  ||  samplerate    != right.samplerate
  ||  blockalign    != right.blockalign
  ||  bitrate       != right.bitrate
  ||  bitspersample != right.bitspersample
  ||  channellayout != right.channellayout)
    return false;

  // SUBTITLE

  // Crypto
  if ((cryptoSession.get() == nullptr) != (right.cryptoSession.get() == nullptr))
    return false;

  if (cryptoSession && !(*cryptoSession.get() == *right.cryptoSession.get()))
    return false;

  return true;
}

bool CDVDStreamInfo::Equal(const CDemuxStream& right, bool withextradata)
{
  CDVDStreamInfo info;
  info.Assign(right, withextradata);
  return Equal(info, withextradata);
}


// ASSIGNMENT
void CDVDStreamInfo::Assign(const CDVDStreamInfo& right, bool withextradata)
{
  codec = right.codec;
  type = right.type;
  uniqueId = right.uniqueId;
  demuxerId = right.demuxerId;
  codec_tag = right.codec_tag;
  flags = right.flags;
  filename = right.filename;
  dvd = right.dvd;

  if( extradata && extrasize ) free(extradata);

  if( withextradata && right.extrasize )
  {
    extrasize = right.extrasize;
    extradata = malloc(extrasize);
    if (!extradata)
      return;
    memcpy(extradata, right.extradata, extrasize);
  }
  else
  {
    extrasize = 0;
    extradata = 0;
  }

  cryptoSession = right.cryptoSession;
  externalInterfaces = right.externalInterfaces;

  // VIDEO
  fpsscale = right.fpsscale;
  fpsrate  = right.fpsrate;
  height   = right.height;
  width    = right.width;
  aspect   = right.aspect;
  stills   = right.stills;
  level    = right.level;
  profile  = right.profile;
  ptsinvalid = right.ptsinvalid;
  forced_aspect = right.forced_aspect;
  orientation = right.orientation;
  bitsperpixel = right.bitsperpixel;
  vfr = right.vfr;
  codecOptions = right.codecOptions;
  stereo_mode = right.stereo_mode;

  // AUDIO
  channels      = right.channels;
  samplerate    = right.samplerate;
  blockalign    = right.blockalign;
  bitrate       = right.bitrate;
  bitspersample = right.bitspersample;
  channellayout = right.channellayout;

  // SUBTITLE
}

void CDVDStreamInfo::Assign(const CDemuxStream& right, bool withextradata)
{
  Clear();

  codec = right.codec;
  type = right.type;
  uniqueId = right.uniqueId;
  demuxerId = right.demuxerId;
  codec_tag = right.codec_fourcc;
  profile = right.profile;
  level = right.level;
  flags = right.flags;

  if (withextradata && right.ExtraSize)
  {
    extrasize = right.ExtraSize;
    extradata = malloc(extrasize);
    if (!extradata)
      return;
    memcpy(extradata, right.ExtraData, extrasize);
  }

  cryptoSession = right.cryptoSession;
  externalInterfaces = right.externalInterfaces;

  if (right.type == STREAM_AUDIO)
  {
    const CDemuxStreamAudio *stream = static_cast<const CDemuxStreamAudio*>(&right);
    channels      = stream->iChannels;
    samplerate    = stream->iSampleRate;
    blockalign    = stream->iBlockAlign;
    bitrate       = stream->iBitRate;
    bitspersample = stream->iBitsPerSample;
    channellayout = stream->iChannelLayout;
  }
  else if (right.type == STREAM_VIDEO)
  {
    const CDemuxStreamVideo *stream = static_cast<const CDemuxStreamVideo*>(&right);
    if (stream->bInterlaced)
      codecOptions |= CODEC_INTERLACED;

    if (stream->bUnknownIP)
      codecOptions |= CODEC_UNKNOWN_I_P;

    fpsscale  = stream->iFpsScale;
    fpsrate   = stream->iFpsRate;
    height    = stream->iHeight;
    width     = stream->iWidth;
    aspect    = stream->fAspect;
    vfr       = stream->bVFR;
    ptsinvalid = stream->bPTSInvalid;
    forced_aspect = stream->bForcedAspect;
    orientation = stream->iOrientation;
    bitsperpixel = stream->iBitsPerPixel;
    stereo_mode = stream->stereo_mode;
  }
  else if (right.type == STREAM_SUBTITLE)
  {
  }
}
