/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace deskflow::audio::macos {

/**
 * Records what the Mac plays, from every process but this one, through a
 * Core Audio process tap (macOS 14.2 and later). Delivers 48 kHz stereo.
 *
 * The tap can also mute what it records on the Mac itself; nothing else
 * changes on the Mac, so the speakers come back as soon as the tap goes,
 * even if this process is killed. The tap follows the default output device.
 */
class SystemAudioCapture
{
public:
  //! Interleaved stereo float at 48 kHz. Called on one Core Audio thread at a time.
  using Sink = std::function<void(const float *pcm, int frames, Clock::time_point captured)>;
  using Log = std::function<void(const QString &line)>;

  SystemAudioCapture(Sink sink, Log log);
  ~SystemAudioCapture();
  SystemAudioCapture(const SystemAudioCapture &) = delete;
  SystemAudioCapture &operator=(const SystemAudioCapture &) = delete;

  bool start(bool muteLocal, QString *error = nullptr);
  void stop();
  bool isRunning() const;

  //! Takes effect at once; the tap is rebuilt, which costs a few milliseconds of audio.
  void setMuteLocal(bool mute);
  //! Capture callbacks so far; stops growing if Core Audio stops delivering.
  std::uint64_t callbacks() const;
  //! Whether the output device is playing anything; the tap is idle while it is not.
  bool outputActive() const;

  //! Remove taps that an earlier instance left behind.
  static void removeStaleTaps(const Log &log);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace deskflow::audio::macos
