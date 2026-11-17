/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/SendStream.h"

#include <algorithm>
#include <cmath>

namespace deskflow::audio {

namespace {

//! -90 dBFS: below this a frame counts as digital silence.
constexpr float kSilenceThreshold = 3.1622776e-5f;

} // namespace

SendStream::SendStream(const StreamFormat &format, std::uint8_t streamId, const StreamKey &key, Transmit transmit)
    : m_format(format),
      m_id(streamId),
      m_key(key),
      m_encoder(format),
      m_sealer(key),
      m_transmit(std::move(transmit))
{
  m_frame.resize(std::size_t(format.frameSamples) * format.channels);
  setSilenceTimeout(std::chrono::seconds(5));
}

void SendStream::setSilenceTimeout(std::chrono::milliseconds timeout)
{
  m_silenceTimeoutFrames = int(timeout.count() * kSampleRate / 1000 / m_format.frameSamples);
}

void SendStream::push(const float *pcm, int frames, Clock::time_point captureTime)
{
  const int channels = m_format.channels;
  if (std::unique_lock lock(m_markMutex, std::try_to_lock); lock.owns_lock()) {
    m_mark = CaptureMark{m_timestamp + std::uint32_t(m_pendingFrames), toMicros(captureTime)};
  }
  while (frames > 0) {
    const int take = std::min(m_format.frameSamples - m_pendingFrames, frames);
    std::copy(
        pcm, pcm + std::size_t(take) * channels, m_frame.begin() + std::ptrdiff_t(m_pendingFrames) * channels
    );
    m_pendingFrames += take;
    pcm += std::size_t(take) * channels;
    frames -= take;
    if (m_pendingFrames == m_format.frameSamples) {
      emitFrame();
      m_pendingFrames = 0;
    }
  }
}

void SendStream::emitFrame()
{
  const auto timestamp = m_timestamp;
  m_timestamp += std::uint32_t(m_format.frameSamples);

  const bool silent =
      std::all_of(m_frame.begin(), m_frame.end(), [](float v) { return std::fabs(v) < kSilenceThreshold; });
  m_silentRun = silent ? m_silentRun + 1 : 0;
  if (m_silenceTimeoutFrames > 0 && m_silentRun > m_silenceTimeoutFrames) {
    m_paused = true;
    m_pausedFlag.store(true, std::memory_order_relaxed);
    m_suppressed.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  MediaHeader header;
  header.streamId = m_id;
  header.sequence = m_sequence++;
  header.timestamp = timestamp;
  if (m_first || m_paused) {
    header.flags |= MediaHeader::Discontinuity;
    m_encoder.reset();
    m_first = false;
    m_paused = false;
    m_pausedFlag.store(false, std::memory_order_relaxed);
  }

  std::uint8_t payload[kMaxPayloadBytes];
  const int size = m_encoder.encode(m_frame.data(), payload, kMaxPayloadBytes);
  if (size <= 0) {
    m_errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::uint8_t datagram[kMaxDatagramBytes];
  const int length = m_sealer.seal(header, payload, size, datagram, kMaxDatagramBytes);
  if (length <= 0) {
    m_errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  m_transmit(datagram, length);
  m_packets.fetch_add(1, std::memory_order_relaxed);
  m_bytes.fetch_add(std::uint64_t(length), std::memory_order_relaxed);
}

std::optional<SendStream::CaptureMark> SendStream::captureMark() const
{
  std::lock_guard lock(m_markMutex);
  return m_mark;
}

SendStats SendStream::stats() const
{
  SendStats stats;
  stats.packets = m_packets.load(std::memory_order_relaxed);
  stats.bytes = m_bytes.load(std::memory_order_relaxed);
  stats.suppressedFrames = m_suppressed.load(std::memory_order_relaxed);
  stats.errors = m_errors.load(std::memory_order_relaxed);
  stats.paused = m_pausedFlag.load(std::memory_order_relaxed);
  return stats;
}

} // namespace deskflow::audio
