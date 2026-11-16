/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace deskflow::audio {

using Clock = std::chrono::steady_clock;

//! Steady clock time in microseconds, as exchanged with the peer (whose clock has another origin).
inline std::int64_t toMicros(Clock::time_point time)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
}

//! All streams run at 48 kHz, the native Opus rate.
inline constexpr int kSampleRate = 48000;

//! Largest encoded frame we accept or send (well under a 1500 byte MTU).
inline constexpr int kMaxPayloadBytes = 1200;

//! Largest datagram on the wire: header + payload + GCM tag.
inline constexpr int kMaxDatagramBytes = 1400;

enum class StreamKind : std::uint8_t
{
  //! Everything the source computer plays, forwarded to another computer.
  System = 1,
  //! A microphone, forwarded to appear as a microphone on another computer.
  Microphone = 2,
};

struct StreamFormat
{
  StreamKind kind = StreamKind::System;
  int channels = 2;
  //! Samples per channel in one packet.
  int frameSamples = 480;
  //! Opus target bitrate in bits per second.
  int bitrate = 192000;

  double frameMs() const
  {
    return frameSamples * 1000.0 / kSampleRate;
  }
};

//! The defaults for each kind: stereo music or mono voice, both Opus CELT at low delay.
StreamFormat defaultFormat(StreamKind kind);

const char *toString(StreamKind kind);

} // namespace deskflow::audio
