/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/macos/SystemAudioCapture.h"

#include "audio/macos/CoreAudioUtil.h"
#include "speex_resampler.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace deskflow::audio::macos {

namespace {

NSString *const kTapName = @"Deskflow system audio";
constexpr UInt32 kCaptureBufferFrames = 240; // 5 ms

AudioObjectID ownProcessObject()
{
  pid_t pid = getpid();
  AudioObjectID process = kAudioObjectUnknown;
  getProperty(
      kAudioObjectSystemObject, kAudioHardwarePropertyTranslatePIDToProcessObject, process,
      kAudioObjectPropertyScopeGlobal, sizeof(pid), &pid
  );
  return process;
}

} // namespace

struct SystemAudioCapture::Impl
{
  Sink sink;
  Log log;
  bool muteLocal = false;
  bool running = false;
  std::atomic<std::uint64_t> callbacks{0};
  mutable std::recursive_mutex mutex; // guards the Core Audio objects below

  AudioObjectID tap = kAudioObjectUnknown;
  AudioObjectID aggregate = kAudioObjectUnknown;
  AudioDeviceIOProcID proc = nullptr;
  dispatch_queue_t ioQueue = nullptr;
  dispatch_queue_t controlQueue = nullptr;
  AudioObjectPropertyListenerBlock outputListener = nil;

  // IO thread state, rebuilt with the tap.
  double rate = kSampleRate;
  int sourceChannels = 2;
  bool interleaved = true;
  SpeexResamplerState_ *resampler = nullptr;
  std::vector<float> frames;
  std::vector<float> resampled;

  bool build(QString *error);
  void teardown();
  void deliver(const AudioBufferList *input, const AudioTimeStamp *inputTime);
};

SystemAudioCapture::SystemAudioCapture(Sink sink, Log log) : m(std::make_unique<Impl>())
{
  m->sink = std::move(sink);
  m->log = std::move(log);
  m->ioQueue = dispatch_queue_create("org.deskflow.audio.tap", DISPATCH_QUEUE_SERIAL);
  m->controlQueue = dispatch_queue_create("org.deskflow.audio.tap-control", DISPATCH_QUEUE_SERIAL);
}

SystemAudioCapture::~SystemAudioCapture()
{
  stop();
}

bool SystemAudioCapture::start(bool muteLocal, QString *error)
{
  std::lock_guard lock(m->mutex);
  if (m->running) {
    return true;
  }
  m->muteLocal = muteLocal;
  if (!m->build(error)) {
    m->teardown();
    return false;
  }
  m->running = true;

  // Rebuild on the new default output device; the old aggregate is tied to the old one.
  Impl *impl = m.get();
  m->outputListener = ^(UInt32, const AudioObjectPropertyAddress *) {
    std::lock_guard relock(impl->mutex);
    if (!impl->running) {
      return;
    }
    impl->log(QStringLiteral("default output device changed, rebuilding the tap"));
    impl->teardown();
    QString reason;
    if (!impl->build(&reason)) {
      impl->log(QStringLiteral("cannot capture system audio: %1").arg(reason));
      impl->teardown();
    }
  };
  AudioObjectPropertyAddress address{
      kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &address, m->controlQueue, m->outputListener);
  return true;
}

void SystemAudioCapture::stop()
{
  AudioObjectPropertyListenerBlock listener = nil;
  {
    std::lock_guard lock(m->mutex);
    if (!m->running) {
      return;
    }
    m->running = false;
    listener = m->outputListener;
    m->outputListener = nil;
    m->teardown();
  }
  if (listener) {
    AudioObjectPropertyAddress address{
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &address, m->controlQueue, listener);
    // A listener call already queued must not run after we return.
    dispatch_sync(m->controlQueue, ^{
                  });
  }
}

bool SystemAudioCapture::isRunning() const
{
  std::lock_guard lock(m->mutex);
  return m->running;
}

void SystemAudioCapture::setMuteLocal(bool mute)
{
  std::lock_guard lock(m->mutex);
  if (m->muteLocal == mute) {
    return;
  }
  m->muteLocal = mute;
  if (!m->running) {
    return;
  }
  m->teardown();
  QString reason;
  if (!m->build(&reason)) {
    m->log(QStringLiteral("cannot capture system audio: %1").arg(reason));
    m->teardown();
  }
}

bool SystemAudioCapture::Impl::build(QString *error)
{
  const auto fail = [error](const QString &what, OSStatus status) {
    if (error) {
      *error = QStringLiteral("%1 failed (%2)").arg(what, statusText(status));
    }
    return false;
  };

  NSArray<NSNumber *> *excluded = @[];
  if (const auto self = ownProcessObject(); self != kAudioObjectUnknown) {
    excluded = @[ @(self) ];
  }
  CATapDescription *description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded];
  description.name = kTapName;
  description.privateTap = YES;
  description.muteBehavior = muteLocal ? CATapMutedWhenTapped : CATapUnmuted;
  OSStatus status = AudioHardwareCreateProcessTap(description, &tap);
  if (status != noErr) {
    tap = kAudioObjectUnknown;
    return fail(QStringLiteral("creating the process tap"), status);
  }

  AudioStreamBasicDescription format{};
  getProperty(tap, kAudioTapPropertyFormat, format);
  rate = format.mSampleRate > 0 ? format.mSampleRate : kSampleRate;
  sourceChannels = format.mChannelsPerFrame > 0 ? int(format.mChannelsPerFrame) : 2;
  interleaved = !(format.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
  if (!(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32) {
    return fail(QStringLiteral("the tap format (not 32-bit float)"), kAudio_UnimplementedError);
  }
  if (int(rate) != kSampleRate) {
    int speexError = 0;
    resampler = speex_resampler_init(2, static_cast<spx_uint32_t>(rate), kSampleRate, 5, &speexError);
  }

  const auto output = defaultOutputDevice();
  const auto outputUid = stringProperty(output, kAudioDevicePropertyDeviceUID);
  if (outputUid.isEmpty()) {
    return fail(QStringLiteral("finding the output device"), kAudioHardwareBadDeviceError);
  }
  NSString *uid = outputUid.toNSString();
  NSDictionary *settings = @{
    @kAudioAggregateDeviceNameKey : kTapName,
    @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
    @kAudioAggregateDeviceMainSubDeviceKey : uid,
    @kAudioAggregateDeviceIsPrivateKey : @YES,
    @kAudioAggregateDeviceIsStackedKey : @NO,
    @kAudioAggregateDeviceTapAutoStartKey : @YES,
    @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : uid} ],
    @kAudioAggregateDeviceTapListKey :
        @[ @{@kAudioSubTapDriftCompensationKey : @YES, @kAudioSubTapUIDKey : description.UUID.UUIDString} ],
  };
  status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)settings, &aggregate);
  if (status != noErr) {
    aggregate = kAudioObjectUnknown;
    return fail(QStringLiteral("creating the capture device"), status);
  }

  // Smaller IO buffers mean the stream leaves in an even flow of packets
  // rather than bursts, which lets the other side keep a smaller buffer.
  UInt32 bufferFrames = kCaptureBufferFrames;
  AudioObjectPropertyAddress bufferAddress{
      kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  AudioObjectSetPropertyData(aggregate, &bufferAddress, 0, nullptr, sizeof(bufferFrames), &bufferFrames);
  bufferFrames = 0;
  getProperty(aggregate, kAudioDevicePropertyBufferFrameSize, bufferFrames);

  Impl *impl = this;
  status = AudioDeviceCreateIOProcIDWithBlock(
      &proc, aggregate, ioQueue,
      ^(const AudioTimeStamp *, const AudioBufferList *input, const AudioTimeStamp *inputTime, AudioBufferList *,
        const AudioTimeStamp *) {
        impl->deliver(input, inputTime);
      }
  );
  if (status != noErr) {
    proc = nullptr;
    return fail(QStringLiteral("registering the capture callback"), status);
  }
  status = AudioDeviceStart(aggregate, proc);
  if (status != noErr) {
    return fail(QStringLiteral("starting capture"), status);
  }
  log(QStringLiteral("capturing system audio from \"%1\" at %2 Hz, %3 frame buffers%4")
          .arg(stringProperty(output, kAudioObjectPropertyName))
          .arg(rate)
          .arg(bufferFrames)
          .arg(muteLocal ? QStringLiteral(", muted on this Mac") : QString()));
  return true;
}

void SystemAudioCapture::Impl::teardown()
{
  if (aggregate != kAudioObjectUnknown && proc) {
    AudioDeviceStop(aggregate, proc);
    AudioDeviceDestroyIOProcID(aggregate, proc);
  }
  proc = nullptr;
  // Callbacks run on ioQueue; let any in flight finish before freeing their state.
  dispatch_sync(ioQueue, ^{
                });
  if (aggregate != kAudioObjectUnknown) {
    AudioHardwareDestroyAggregateDevice(aggregate);
    aggregate = kAudioObjectUnknown;
  }
  if (tap != kAudioObjectUnknown) {
    AudioHardwareDestroyProcessTap(tap);
    tap = kAudioObjectUnknown;
  }
  if (resampler) {
    speex_resampler_destroy(resampler);
    resampler = nullptr;
  }
}

bool SystemAudioCapture::outputActive() const
{
  UInt32 running = 0;
  getProperty(defaultOutputDevice(), kAudioDevicePropertyDeviceIsRunningSomewhere, running);
  return running != 0;
}

std::uint64_t SystemAudioCapture::callbacks() const
{
  return m->callbacks.load(std::memory_order_relaxed);
}

void SystemAudioCapture::Impl::deliver(const AudioBufferList *input, const AudioTimeStamp *inputTime)
{
  callbacks.fetch_add(1, std::memory_order_relaxed);
  const int count = interleave(input, interleaved, sourceChannels, 2, frames);
  if (count <= 0) {
    return;
  }
  const auto captured = fromHostTime(inputTime);
  if (!resampler) {
    sink(frames.data(), count, captured);
    return;
  }
  resampled.resize(std::size_t(count * kSampleRate / rate + 64) * 2);
  auto inLength = static_cast<spx_uint32_t>(count);
  auto outLength = static_cast<spx_uint32_t>(resampled.size() / 2);
  speex_resampler_process_interleaved_float(resampler, frames.data(), &inLength, resampled.data(), &outLength);
  if (outLength > 0) {
    sink(resampled.data(), int(outLength), captured);
  }
}

void SystemAudioCapture::removeStaleTaps(const Log &log)
{
  AudioObjectPropertyAddress address{
      kAudioHardwarePropertyTapList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
    return;
  }
  std::vector<AudioObjectID> taps(size / sizeof(AudioObjectID));
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, taps.data());
  for (const auto tap : taps) {
    CFTypeRef raw = nullptr;
    getProperty(tap, kAudioTapPropertyDescription, raw);
    CATapDescription *description = raw ? (CATapDescription *)CFBridgingRelease(raw) : nil;
    if ([description.name isEqualToString:kTapName]) {
      const auto status = AudioHardwareDestroyProcessTap(tap);
      log(QStringLiteral("removed a stale tap left by an earlier run (%1)").arg(statusText(status)));
    }
  }
}

} // namespace deskflow::audio::macos
