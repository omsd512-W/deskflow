/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// The "Deskflow 麦克风" virtual microphone, a Core Audio HAL plug-in.
//
// Two devices share one clock: a hidden "feed" output that deskflow-audio
// plays the other computer's microphone into, and a visible input that apps
// record from. The two-device design follows Oink's MIT-licensed HAL plug-in
// (github.com/oinkvm/oink, apps/audio-plugin, Copyright (c) Oink
// contributors); no code is taken from it. Built on libASPL (MIT).
//
// The UIDs are what deskflow-audio looks for (audio/macos/VirtualMicrophone.mm);
// they must not change, or apps lose the device they were set to use.

#include <aspl/Driver.hpp>

#include <mach/mach_time.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr UInt32 kRate = 48000;
constexpr UInt32 kPeriod = 256;
// Frames between the feed writing a sample and the input handing it out.
constexpr UInt64 kDelay = DESKFLOW_MIC_DELAY;

// Non-destructive ring addressed by absolute frame number: one writer (the
// feed's mixed output), any number of readers. No locks or allocation.
class Ring
{
public:
  static constexpr UInt64 kCapacity = 16384;

  void write(UInt64 frame, float value)
  {
    auto &slot = m_slots[frame % kCapacity];
    slot.frame.store(kInvalid, std::memory_order_relaxed);
    UInt32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    slot.bits.store(bits, std::memory_order_relaxed);
    slot.frame.store(frame, std::memory_order_release);
  }

  float read(UInt64 frame) const
  {
    const auto &slot = m_slots[frame % kCapacity];
    if (slot.frame.load(std::memory_order_acquire) != frame) {
      return 0;
    }
    const UInt32 bits = slot.bits.load(std::memory_order_relaxed);
    if (slot.frame.load(std::memory_order_acquire) != frame) {
      return 0;
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void clear()
  {
    for (auto &slot : m_slots) {
      slot.frame.store(kInvalid, std::memory_order_relaxed);
    }
  }

private:
  static constexpr UInt64 kInvalid = std::numeric_limits<UInt64>::max();
  struct Slot
  {
    std::atomic<UInt64> frame{kInvalid};
    std::atomic<UInt32> bits{0};
  };
  static_assert(std::atomic<UInt64>::is_always_lock_free);
  std::array<Slot, kCapacity> m_slots{};
};

struct Shared
{
  Ring ring;
  std::atomic<bool> feeding{false};
  const UInt64 epoch = mach_absolute_time();
  double ticksPerFrame = 0;

  Shared()
  {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    ticksPerFrame = 1e9 * timebase.denom / timebase.numer / kRate;
  }
};

class Handler final : public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
  Handler(std::shared_ptr<Shared> shared, bool input) : m_shared(std::move(shared)), m_input(input)
  {
  }

  OSStatus OnStartIO() override
  {
    if (!m_input) {
      m_shared->ring.clear();
      m_shared->feeding.store(true);
    }
    return kAudioHardwareNoError;
  }

  void OnStopIO() override
  {
    if (!m_input) {
      m_shared->feeding.store(false);
      m_shared->ring.clear();
    }
  }

  void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream> &, Float64, Float64 timestamp, const void *bytes,
                          UInt32 count) override
  {
    if (m_input || timestamp < 0 || !std::isfinite(timestamp)) {
      return;
    }
    const auto *samples = static_cast<const float *>(bytes);
    const auto first = static_cast<UInt64>(timestamp);
    for (UInt32 i = 0; i < count / sizeof(float); ++i) {
      m_shared->ring.write(first + i, samples[i]);
    }
  }

  void OnReadClientInput(const std::shared_ptr<aspl::Client> &, const std::shared_ptr<aspl::Stream> &, Float64,
                         Float64 timestamp, void *bytes, UInt32 count) override
  {
    std::memset(bytes, 0, count);
    if (!m_input || !m_shared->feeding.load() || !std::isfinite(timestamp) || timestamp < kDelay) {
      return;
    }
    auto *samples = static_cast<float *>(bytes);
    const auto first = static_cast<UInt64>(timestamp) - kDelay;
    for (UInt32 i = 0; i < count / sizeof(float); ++i) {
      samples[i] = m_shared->ring.read(first + i);
    }
  }

private:
  std::shared_ptr<Shared> m_shared;
  bool m_input;
};

class Device final : public aspl::Device
{
public:
  Device(std::shared_ptr<aspl::Context> context, const aspl::DeviceParameters &params, std::shared_ptr<Shared> shared,
         bool hidden)
      : aspl::Device(std::move(context), params),
        m_shared(std::move(shared)),
        m_hidden(hidden)
  {
  }

  bool GetIsHidden() const override
  {
    return m_hidden;
  }

protected:
  // Both devices must report the same timeline, even if started minutes apart,
  // so that a feed sample written at frame N is read back at frame N + delay.
  OSStatus GetZeroTimeStampImpl(UInt32, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) override
  {
    const double frames = (mach_absolute_time() - m_shared->epoch) / m_shared->ticksPerFrame;
    const auto periods = static_cast<UInt64>(frames / kPeriod);
    *sampleTime = static_cast<Float64>(periods * kPeriod);
    *hostTime = m_shared->epoch + static_cast<UInt64>(*sampleTime * m_shared->ticksPerFrame);
    *seed = 1;
    return kAudioHardwareNoError;
  }

private:
  std::shared_ptr<Shared> m_shared;
  bool m_hidden;
};

std::shared_ptr<aspl::Driver> createDriver()
{
  auto context = std::make_shared<aspl::Context>(std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Noop));
  auto plugin = std::make_shared<aspl::Plugin>(context);
  auto shared = std::make_shared<Shared>();

  for (const bool input : {true, false}) {
    aspl::DeviceParameters params;
    params.Name = input ? "Deskflow 麦克风" : "Deskflow 麦克风（内部）";
    params.Manufacturer = "Deskflow";
    params.DeviceUID = input ? "org.deskflow.microphone.input.v1" : "org.deskflow.microphone.feed.v1";
    params.ModelUID = "org.deskflow.microphone.v1";
    params.SampleRate = kRate;
    params.ChannelCount = 1;
    params.ZeroTimeStampPeriod = kPeriod;
    params.ClockDomain = 0x44736b66; // 'Dskf', shared by both devices
    params.Latency = input ? static_cast<UInt32>(kDelay) : 0;
    params.CanBeDefault = input;
    params.CanBeDefaultForSystemSounds = false;

    auto device = std::make_shared<Device>(context, params, shared, !input);

    aspl::StreamParameters stream;
    stream.Direction = input ? aspl::Direction::Input : aspl::Direction::Output;
    stream.Format = {kRate, kAudioFormatLinearPCM,
                     kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
                     sizeof(float), 1, sizeof(float), 1, 32, 0};
    device->AddStreamAsync(stream);

    auto handler = std::make_shared<Handler>(shared, input);
    device->SetControlHandler(handler);
    device->SetIOHandler(handler);
    plugin->AddDevice(device);
  }
  return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void *DeskflowMicrophoneFactory(CFAllocatorRef, CFUUIDRef type)
{
  if (type == nullptr || !CFEqual(type, kAudioServerPlugInTypeUUID)) {
    return nullptr;
  }
  static auto driver = createDriver();
  return driver->GetReference();
}
