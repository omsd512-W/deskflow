/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"
#include "audio/MediaPacket.h"
#include "audio/OpusCodec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace deskflow::audio {

struct SendStats
{
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t suppressedFrames = 0; //!< silent frames not sent
  std::uint64_t errors = 0;
  bool paused = false; //!< currently silent and not sending
};

/**
 * Send side of one stream: cuts captured PCM into frames, encodes, seals and
 * hands each datagram to the transmit function. Call push() from one thread.
 *
 * After a run of digital silence it stops sending; the sample clock keeps
 * running, and the first packet afterwards carries the Discontinuity flag.
 */
class SendStream
{
public:
  using Transmit = std::function<void(const std::uint8_t *datagram, int size)>;

  SendStream(const StreamFormat &format, std::uint8_t streamId, const StreamKey &key, Transmit transmit);

  std::uint8_t id() const
  {
    return m_id;
  }
  const StreamFormat &format() const
  {
    return m_format;
  }
  const StreamKey &key() const
  {
    return m_key;
  }

  //! Interleaved float samples, any number of frames, the first of them
  //! captured at captureTime.
  void push(const float *pcm, int frames, Clock::time_point captureTime = Clock::now());

  struct CaptureMark
  {
    std::uint32_t timestamp = 0; //!< stream sample clock
    std::int64_t captureUs = 0;  //!< when that sample was captured, toMicros()
  };
  //! The latest sample whose capture time is known. Any thread.
  std::optional<CaptureMark> captureMark() const;

  //! Zero disables silence suppression.
  void setSilenceTimeout(std::chrono::milliseconds timeout);

  SendStats stats() const;

private:
  void emitFrame();

  StreamFormat m_format;
  std::uint8_t m_id;
  StreamKey m_key;
  Encoder m_encoder;
  PacketSealer m_sealer;
  Transmit m_transmit;

  std::vector<float> m_frame;
  int m_pendingFrames = 0;
  std::uint64_t m_sequence = 0;
  std::uint32_t m_timestamp = 0;
  bool m_first = true;
  bool m_paused = false;
  int m_silentRun = 0;
  int m_silenceTimeoutFrames = 0;

  std::atomic<std::uint64_t> m_packets{0};
  std::atomic<std::uint64_t> m_bytes{0};
  std::atomic<std::uint64_t> m_suppressed{0};
  std::atomic<std::uint64_t> m_errors{0};
  std::atomic<bool> m_pausedFlag{false};

  mutable std::mutex m_markMutex;
  std::optional<CaptureMark> m_mark;
};

} // namespace deskflow::audio
