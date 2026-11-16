/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

namespace deskflow::audio {

/**
 * A sound card opened through miniaudio (WASAPI shared mode on Windows),
 * always at 48 kHz float with the given channel count; miniaudio converts
 * to whatever the device runs at.
 *
 * With no device name it follows the system default device, including when
 * the default changes. If the device goes away the device stops; call
 * start() again (isRunning() turns false).
 */
class AudioDevice
{
public:
  enum class Direction
  {
    Playback,
    Capture
  };
  //! Playback: fill `frames` interleaved frames. Called on the device thread.
  using Source = std::function<void(float *out, int frames)>;
  //! Capture: `frames` interleaved frames, captured starting at `captured`.
  using Sink = std::function<void(const float *in, int frames, Clock::time_point captured)>;
  using Log = std::function<void(const QString &line)>;

  AudioDevice(Direction direction, int channels, Log log);
  ~AudioDevice();
  AudioDevice(const AudioDevice &) = delete;
  AudioDevice &operator=(const AudioDevice &) = delete;

  void setSource(Source source);
  void setSink(Sink sink);

  //! @param name device name as listed by devices(); empty for the system default
  bool start(const QString &name = {}, QString *error = nullptr);
  void stop();
  bool isRunning() const;
  QString deviceName() const;
  //! Roughly how long the device takes from handing it audio to playing it
  //! (or from hearing to delivering it), from its buffer size.
  double latencyMs() const;
  //! Data callbacks so far; stops growing if the device stops running.
  std::uint64_t callbacks() const;

  static QStringList devices(Direction direction);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace deskflow::audio
