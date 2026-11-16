/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"

#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <QString>

#include <string>
#include <vector>

namespace deskflow::audio::macos {

template <typename T>
bool getProperty(
    AudioObjectID object, AudioObjectPropertySelector selector, T &value,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, UInt32 qualifierSize = 0,
    const void *qualifier = nullptr
)
{
  AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(T);
  return AudioObjectGetPropertyData(object, &address, qualifierSize, qualifier, &size, &value) == noErr;
}

inline QString stringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
  CFStringRef value = nullptr;
  if (!getProperty(object, selector, value) || value == nullptr) {
    return {};
  }
  const auto result = QString::fromCFString(value);
  CFRelease(value);
  return result;
}

inline AudioObjectID deviceForUid(const QString &uid)
{
  // Unlike walking kAudioHardwarePropertyDevices, this finds hidden devices too.
  CFStringRef key = uid.toCFString();
  AudioObjectID device = kAudioObjectUnknown;
  getProperty(
      kAudioObjectSystemObject, kAudioHardwarePropertyTranslateUIDToDevice, device, kAudioObjectPropertyScopeGlobal,
      sizeof(key), &key
  );
  CFRelease(key);
  return device;
}

inline AudioObjectID defaultOutputDevice()
{
  AudioObjectID device = kAudioObjectUnknown;
  getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, device);
  return device;
}

inline QString statusText(OSStatus status)
{
  const auto code = std::uint32_t(status);
  char text[5] = {char(code >> 24), char(code >> 16), char(code >> 8), char(code), 0};
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    printable = printable && text[i] >= 32 && text[i] < 127;
  }
  return printable ? QStringLiteral("'%1'").arg(QString::fromLatin1(text)) : QString::number(status);
}

//! When a Core Audio host time happened, on our steady clock.
inline Clock::time_point fromHostTime(const AudioTimeStamp *time)
{
  const auto now = Clock::now();
  if (!time || !(time->mFlags & kAudioTimeStampHostTimeValid)) {
    return now;
  }
  const auto host = mach_absolute_time();
  if (time->mHostTime > host) {
    return now + std::chrono::nanoseconds(AudioConvertHostTimeToNanos(time->mHostTime - host));
  }
  return now - std::chrono::nanoseconds(AudioConvertHostTimeToNanos(host - time->mHostTime));
}

//! Copies a buffer list in any channel layout into interleaved float frames
//! with `channels` channels (extra channels dropped, missing ones repeated).
//! @return frames copied
int interleave(const AudioBufferList *list, bool interleaved, int sourceChannels, int channels, std::vector<float> &out);

} // namespace deskflow::audio::macos
