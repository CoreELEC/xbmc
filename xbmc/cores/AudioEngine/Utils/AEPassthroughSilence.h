/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AEStreamInfo.h"

#include <stdint.h>

struct AEPassthroughSilentFrame
{
  const uint8_t* data;
  unsigned int size;
};

class CAEPassthroughSilence
{
public:
  /*! \brief Find the encoded silent frame for the format of a raw frame
   \param type the stream type of the frame
   \param data the frame as the stream carries it
   \param size the size of the frame in bytes
   \return the silent frame, or nullptr if there is none for this format
   */
  static const AEPassthroughSilentFrame* Find(CAEStreamInfo::DataType type,
                                              const uint8_t* data,
                                              unsigned int size);
};
