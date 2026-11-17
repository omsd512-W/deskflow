/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/OpusCodec.h"

#include <opus.h>

namespace deskflow::audio {

Encoder::Encoder(const StreamFormat &format) : m_format(format)
{
  const bool voice = format.kind == StreamKind::Microphone;
  // RESTRICTED_LOWDELAY is CELT only: 2.5 ms lookahead instead of 6.5 ms,
  // and its loss concealment joins cleanly onto the packets that follow.
  // (SILK, the only mode with in-band FEC, leaves a transient after every
  // loss whether or not FEC rebuilt the frame, so FEC is not worth having.)
  const int application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  int error = OPUS_OK;
  m_encoder = opus_encoder_create(kSampleRate, format.channels, application, &error);
  if (error != OPUS_OK) {
    m_encoder = nullptr;
    return;
  }
  opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(format.bitrate));
  opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
  opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(voice ? 9 : 8));
  opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(voice ? OPUS_SIGNAL_VOICE : OPUS_SIGNAL_MUSIC));
}

Encoder::~Encoder()
{
  if (m_encoder) {
    opus_encoder_destroy(m_encoder);
  }
}

int Encoder::encode(const float *pcm, std::uint8_t *out, int capacity)
{
  if (!m_encoder) {
    return -1;
  }
  const auto size = opus_encode_float(m_encoder, pcm, m_format.frameSamples, out, capacity);
  return size < 0 ? -1 : size;
}

void Encoder::reset()
{
  if (m_encoder) {
    opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
  }
}

int Encoder::lookahead() const
{
  opus_int32 samples = 0;
  if (m_encoder) {
    opus_encoder_ctl(m_encoder, OPUS_GET_LOOKAHEAD(&samples));
  }
  return samples;
}

Decoder::Decoder(const StreamFormat &format) : m_format(format)
{
  int error = OPUS_OK;
  m_decoder = opus_decoder_create(kSampleRate, format.channels, &error);
  if (error != OPUS_OK) {
    m_decoder = nullptr;
  }
}

Decoder::~Decoder()
{
  if (m_decoder) {
    opus_decoder_destroy(m_decoder);
  }
}

int Decoder::decode(const std::uint8_t *data, int size, float *pcm, int frames)
{
  if (!m_decoder) {
    return -1;
  }
  const auto samples = opus_decode_float(m_decoder, data, size, pcm, frames, 0);
  return samples < 0 ? -1 : samples;
}

int Decoder::conceal(float *pcm, int frames)
{
  if (!m_decoder) {
    return -1;
  }
  const auto samples = opus_decode_float(m_decoder, nullptr, 0, pcm, frames, 0);
  return samples < 0 ? -1 : samples;
}

void Decoder::reset()
{
  if (m_decoder) {
    opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
  }
}

} // namespace deskflow::audio
