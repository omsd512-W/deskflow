/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/macos/CoreAudioUtil.h"

#include <algorithm>

namespace deskflow::audio::macos {

int interleave(const AudioBufferList *list, bool interleaved, int sourceChannels, int channels, std::vector<float> &out)
{
  if (!list || list->mNumberBuffers == 0 || sourceChannels <= 0) {
    return 0;
  }
  if (interleaved) {
    const auto &buffer = list->mBuffers[0];
    const int stride = int(buffer.mNumberChannels);
    if (stride <= 0 || !buffer.mData) {
      return 0;
    }
    const int frames = int(buffer.mDataByteSize / sizeof(float)) / stride;
    const auto *in = static_cast<const float *>(buffer.mData);
    out.resize(std::size_t(frames) * channels);
    for (int f = 0; f < frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        out[std::size_t(f) * channels + c] = in[std::size_t(f) * stride + std::min(c, stride - 1)];
      }
    }
    return frames;
  }
  // One buffer per channel.
  const int buffers = int(list->mNumberBuffers);
  const int frames = int(list->mBuffers[0].mDataByteSize / sizeof(float));
  out.resize(std::size_t(frames) * channels);
  for (int c = 0; c < channels; ++c) {
    const auto &buffer = list->mBuffers[std::min(c, buffers - 1)];
    const auto *in = static_cast<const float *>(buffer.mData);
    if (!in) {
      return 0;
    }
    for (int f = 0; f < frames; ++f) {
      out[std::size_t(f) * channels + c] = in[f];
    }
  }
  return frames;
}

} // namespace deskflow::audio::macos
