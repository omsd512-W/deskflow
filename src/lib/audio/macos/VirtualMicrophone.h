/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace deskflow::audio::macos {

/**
 * The "Deskflow Microphone" input device, provided by the
 * DeskflowMicrophone.driver HAL plug-in. What is written to its hidden feed
 * device comes out of the visible input device, for any app to record.
 */
class VirtualMicrophone
{
public:
  //! Fill `frames` mono 48 kHz samples. Called on a Core Audio thread.
  using Source = std::function<void(float *out, int frames)>;
  //! Some app started or stopped recording from the input device.
  using InUseChanged = std::function<void(bool inUse)>;
  using Log = std::function<void(const QString &line)>;

  VirtualMicrophone(Source source, InUseChanged inUseChanged, Log log);
  ~VirtualMicrophone();
  VirtualMicrophone(const VirtualMicrophone &) = delete;
  VirtualMicrophone &operator=(const VirtualMicrophone &) = delete;

  static bool isInstalled();

  //! Start watching whether the input device is in use.
  bool watch();
  bool isInUse() const;
  //! Follow the devices if they were recreated (the driver was reinstalled or
  //! coreaudiod restarted). Returns whether they exist.
  bool refresh();

  //! Start or stop feeding audio.
  bool startFeeding(QString *error = nullptr);
  void stopFeeding();
  bool isFeeding() const;
  //! Feed callbacks so far; stops growing if Core Audio stops asking.
  std::uint64_t callbacks() const;
  //! From handing the feed a frame to an app receiving it.
  double latencyMs() const;

private:
  void unwatch();
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace deskflow::audio::macos
