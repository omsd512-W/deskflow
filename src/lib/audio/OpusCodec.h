/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"

#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace deskflow::audio {

//! Encodes fixed-size frames of interleaved float PCM.
class Encoder
{
public:
  explicit Encoder(const StreamFormat &format);
  ~Encoder();
  Encoder(const Encoder &) = delete;
  Encoder &operator=(const Encoder &) = delete;

  bool isValid() const
  {
    return m_encoder != nullptr;
  }

  //! @param pcm exactly frameSamples * channels samples
  //! @return encoded size in bytes, or -1 on error
  int encode(const float *pcm, std::uint8_t *out, int capacity);

  //! Forget all history, e.g. before the first frame after a pause.
  void reset();

  //! Algorithmic delay added by the encoder, in samples per channel.
  int lookahead() const;

private:
  ::OpusEncoder *m_encoder = nullptr;
  StreamFormat m_format;
};

//! Decodes packets back to interleaved float PCM, and hides lost ones.
class Decoder
{
public:
  explicit Decoder(const StreamFormat &format);
  ~Decoder();
  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;

  bool isValid() const
  {
    return m_decoder != nullptr;
  }

  //! @return samples per channel written to pcm, or -1 on error
  int decode(const std::uint8_t *data, int size, float *pcm, int frames);

  //! Synthesize a frame for a packet that never arrived.
  int conceal(float *pcm, int frames);

  void reset();

private:
  ::OpusDecoder *m_decoder = nullptr;
  StreamFormat m_format;
};

} // namespace deskflow::audio
