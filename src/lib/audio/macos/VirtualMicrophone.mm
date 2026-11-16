/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/macos/VirtualMicrophone.h"

#include "audio/AudioTypes.h"
#include "audio/macos/CoreAudioUtil.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace deskflow::audio::macos {

namespace {

// Must match the plug-in (src/apps/deskflow-mic-driver).
const auto kFeedUid = QStringLiteral("org.deskflow.microphone.feed.v1");
const auto kInputUid = QStringLiteral("org.deskflow.microphone.input.v1");

bool runningSomewhere(AudioObjectID device)
{
  UInt32 running = 0;
  getProperty(device, kAudioDevicePropertyDeviceIsRunningSomewhere, running);
  return running != 0;
}

UInt32 latencyFrames(AudioObjectID device, AudioObjectPropertyScope scope)
{
  UInt32 latency = 0;
  UInt32 safety = 0;
  getProperty(device, kAudioDevicePropertyLatency, latency, scope);
  getProperty(device, kAudioDevicePropertySafetyOffset, safety, scope);
  UInt32 buffer = 0;
  getProperty(device, kAudioDevicePropertyBufferFrameSize, buffer, scope);
  return latency + safety + buffer;
}

} // namespace

struct VirtualMicrophone::Impl
{
  Source source;
  InUseChanged inUseChanged;
  Log log;
  mutable std::mutex mutex;

  AudioObjectID input = kAudioObjectUnknown;
  AudioObjectID feed = kAudioObjectUnknown;
  dispatch_queue_t ioQueue = nullptr;
  dispatch_queue_t controlQueue = nullptr;
  AudioObjectPropertyListenerBlock listener = nil;
  std::atomic<bool> inUse{false};
  AudioDeviceIOProcID proc = nullptr;
  bool feeding = false;
  std::atomic<std::uint64_t> callbacks{0};
  std::vector<float> mono;

  void render(AudioBufferList *output);
};

VirtualMicrophone::VirtualMicrophone(Source source, InUseChanged inUseChanged, Log log)
    : m(std::make_unique<Impl>())
{
  m->source = std::move(source);
  m->inUseChanged = std::move(inUseChanged);
  m->log = std::move(log);
  m->ioQueue = dispatch_queue_create("org.deskflow.audio.microphone", DISPATCH_QUEUE_SERIAL);
  m->controlQueue = dispatch_queue_create("org.deskflow.audio.microphone-control", DISPATCH_QUEUE_SERIAL);
}

VirtualMicrophone::~VirtualMicrophone()
{
  stopFeeding();
  unwatch();
}

void VirtualMicrophone::unwatch()
{
  AudioObjectPropertyListenerBlock listener = nil;
  AudioObjectID input = kAudioObjectUnknown;
  {
    std::lock_guard lock(m->mutex);
    listener = m->listener;
    input = m->input;
    m->listener = nil;
  }
  if (listener) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListenerBlock(input, &address, m->controlQueue, listener);
    dispatch_sync(m->controlQueue, ^{
                  });
  }
}

bool VirtualMicrophone::refresh()
{
  const auto input = deviceForUid(kInputUid);
  const auto feed = deviceForUid(kFeedUid);
  {
    std::lock_guard lock(m->mutex);
    if (input == m->input && feed == m->feed && (m->listener || input == kAudioObjectUnknown)) {
      return input != kAudioObjectUnknown && feed != kAudioObjectUnknown;
    }
  }
  m->log(QStringLiteral("the Deskflow microphone devices changed, following them"));
  const bool wasFeeding = isFeeding();
  stopFeeding();
  unwatch();
  {
    std::lock_guard lock(m->mutex);
    m->input = kAudioObjectUnknown;
    m->feed = kAudioObjectUnknown;
  }
  if (m->inUse.exchange(false)) {
    m->inUseChanged(false);
  }
  if (!watch()) {
    return false;
  }
  if (wasFeeding) {
    startFeeding();
  }
  return true;
}

bool VirtualMicrophone::isInstalled()
{
  return deviceForUid(kFeedUid) != kAudioObjectUnknown && deviceForUid(kInputUid) != kAudioObjectUnknown;
}

bool VirtualMicrophone::watch()
{
  std::lock_guard lock(m->mutex);
  if (m->listener) {
    return true;
  }
  m->input = deviceForUid(kInputUid);
  m->feed = deviceForUid(kFeedUid);
  if (m->input == kAudioObjectUnknown || m->feed == kAudioObjectUnknown) {
    return false;
  }
  Impl *impl = m.get();
  m->listener = ^(UInt32, const AudioObjectPropertyAddress *) {
    const bool now = runningSomewhere(impl->input);
    if (impl->inUse.exchange(now) != now) {
      impl->inUseChanged(now);
    }
  };
  AudioObjectPropertyAddress address{
      kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  AudioObjectAddPropertyListenerBlock(m->input, &address, m->controlQueue, m->listener);
  m->inUse = runningSomewhere(m->input);
  if (m->inUse) {
    m->inUseChanged(true);
  }
  return true;
}

bool VirtualMicrophone::isInUse() const
{
  return m->inUse;
}

bool VirtualMicrophone::startFeeding(QString *error)
{
  std::lock_guard lock(m->mutex);
  if (m->feeding) {
    return true;
  }
  if (m->feed == kAudioObjectUnknown) {
    m->feed = deviceForUid(kFeedUid);
  }
  if (m->feed == kAudioObjectUnknown) {
    if (error) {
      *error = QStringLiteral("the Deskflow microphone driver is not installed");
    }
    return false;
  }
  Impl *impl = m.get();
  auto status = AudioDeviceCreateIOProcIDWithBlock(
      &m->proc, m->feed, m->ioQueue,
      ^(const AudioTimeStamp *, const AudioBufferList *, const AudioTimeStamp *, AudioBufferList *output,
        const AudioTimeStamp *) {
        impl->render(output);
      }
  );
  if (status == noErr) {
    status = AudioDeviceStart(m->feed, m->proc);
  }
  if (status != noErr) {
    if (m->proc) {
      AudioDeviceDestroyIOProcID(m->feed, m->proc);
      m->proc = nullptr;
    }
    if (error) {
      *error = QStringLiteral("cannot start the microphone feed (%1)").arg(statusText(status));
    }
    return false;
  }
  m->feeding = true;
  m->log(QStringLiteral("feeding the Deskflow microphone"));
  return true;
}

void VirtualMicrophone::stopFeeding()
{
  std::lock_guard lock(m->mutex);
  if (!m->feeding) {
    return;
  }
  AudioDeviceStop(m->feed, m->proc);
  AudioDeviceDestroyIOProcID(m->feed, m->proc);
  m->proc = nullptr;
  dispatch_sync(m->ioQueue, ^{
                });
  m->feeding = false;
  m->log(QStringLiteral("stopped feeding the Deskflow microphone"));
}

bool VirtualMicrophone::isFeeding() const
{
  std::lock_guard lock(m->mutex);
  return m->feeding;
}

double VirtualMicrophone::latencyMs() const
{
  std::lock_guard lock(m->mutex);
  if (m->feed == kAudioObjectUnknown || m->input == kAudioObjectUnknown) {
    return 0;
  }
  const auto frames = latencyFrames(m->feed, kAudioObjectPropertyScopeOutput) +
                      latencyFrames(m->input, kAudioObjectPropertyScopeInput);
  return frames * 1000.0 / kSampleRate;
}

std::uint64_t VirtualMicrophone::callbacks() const
{
  return m->callbacks.load(std::memory_order_relaxed);
}

void VirtualMicrophone::Impl::render(AudioBufferList *output)
{
  callbacks.fetch_add(1, std::memory_order_relaxed);
  if (!output || output->mNumberBuffers == 0) {
    return;
  }
  auto &first = output->mBuffers[0];
  const int stride = int(first.mNumberChannels) > 0 ? int(first.mNumberChannels) : 1;
  const int frames = int(first.mDataByteSize / sizeof(float)) / stride;
  mono.resize(std::size_t(frames));
  source(mono.data(), frames);
  // A loud microphone can decode slightly past full scale; apps expect [-1, 1].
  for (auto &sample : mono) {
    sample = std::clamp(sample, -1.0f, 1.0f);
  }
  for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
    auto &buffer = output->mBuffers[b];
    auto *out = static_cast<float *>(buffer.mData);
    const int channels = int(buffer.mNumberChannels) > 0 ? int(buffer.mNumberChannels) : 1;
    if (!out) {
      continue;
    }
    for (int f = 0; f < frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        out[std::size_t(f) * channels + c] = mono[std::size_t(f)];
      }
    }
  }
}

} // namespace deskflow::audio::macos
