/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/AudioDevice.h"

#include "MiniaudioConfig.h"
#include "miniaudio.h"

#include <mutex>

namespace deskflow::audio {

struct AudioDevice::Impl
{
  Direction direction;
  int channels;
  Log log;
  Source source;
  Sink sink;

  mutable std::mutex mutex; // guards the members below
  bool contextReady = false;
  ma_context context{};
  bool open = false;
  ma_device device{};
  QString name;
  std::atomic<bool> running{false};
  std::atomic<double> latency{0};
  std::atomic<std::uint64_t> callbacks{0};

  static void onData(ma_device *device, void *output, const void *input, ma_uint32 frames);
  static void onNotification(const ma_device_notification *notification);
  void updateLatency();
};

namespace {

ma_device_type toMiniaudio(AudioDevice::Direction direction)
{
  return direction == AudioDevice::Direction::Playback ? ma_device_type_playback : ma_device_type_capture;
}

QString resultText(ma_result result)
{
  return QString::fromUtf8(ma_result_description(result));
}

} // namespace

AudioDevice::AudioDevice(Direction direction, int channels, Log log) : m(std::make_unique<Impl>())
{
  m->direction = direction;
  m->channels = channels;
  m->log = std::move(log);
}

AudioDevice::~AudioDevice()
{
  stop();
  if (m->contextReady) {
    ma_context_uninit(&m->context);
  }
}

void AudioDevice::setSource(Source source)
{
  m->source = std::move(source);
}

void AudioDevice::setSink(Sink sink)
{
  m->sink = std::move(sink);
}

bool AudioDevice::start(const QString &name, QString *error)
{
  std::lock_guard lock(m->mutex);
  if (m->open) {
    if (m->running) {
      return true;
    }
    // Stopped by itself (device lost): open it again from scratch.
    ma_device_uninit(&m->device);
    m->open = false;
  }
  if (!m->contextReady) {
    if (const auto result = ma_context_init(nullptr, 0, nullptr, &m->context); result != MA_SUCCESS) {
      if (error) {
        *error = QStringLiteral("no audio system: %1").arg(resultText(result));
      }
      return false;
    }
    m->contextReady = true;
  }

  const auto type = toMiniaudio(m->direction);
  ma_device_id chosen{};
  bool haveChosen = false;
  if (!name.isEmpty()) {
    ma_device_info *playback = nullptr;
    ma_device_info *capture = nullptr;
    ma_uint32 playbackCount = 0;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&m->context, &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS) {
      const auto *list = type == ma_device_type_playback ? playback : capture;
      const auto count = type == ma_device_type_playback ? playbackCount : captureCount;
      for (ma_uint32 i = 0; i < count; ++i) {
        if (QString::fromUtf8(list[i].name) == name) {
          chosen = list[i].id;
          haveChosen = true;
        }
      }
    }
    if (!haveChosen) {
      m->log(QStringLiteral("device \"%1\" not found, using the default").arg(name));
    }
  }

  ma_device_config config = ma_device_config_init(type);
  config.sampleRate = kSampleRate;
  config.periodSizeInMilliseconds = 10;
  config.periods = 2;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.noPreSilencedOutputBuffer = MA_FALSE;
  config.noClip = MA_TRUE;
  config.dataCallback = &Impl::onData;
  config.notificationCallback = &Impl::onNotification;
  config.pUserData = m.get();
  if (type == ma_device_type_playback) {
    config.playback.format = ma_format_f32;
    config.playback.channels = ma_uint32(m->channels);
    config.playback.pDeviceID = haveChosen ? &chosen : nullptr;
    config.playback.shareMode = ma_share_mode_shared;
  } else {
    config.capture.format = ma_format_f32;
    config.capture.channels = ma_uint32(m->channels);
    config.capture.pDeviceID = haveChosen ? &chosen : nullptr;
    config.capture.shareMode = ma_share_mode_shared;
  }
  if (const auto result = ma_device_init(&m->context, &config, &m->device); result != MA_SUCCESS) {
    if (error) {
      *error = QStringLiteral("cannot open the %1 device: %2")
                   .arg(type == ma_device_type_playback ? QStringLiteral("playback") : QStringLiteral("recording"))
                   .arg(resultText(result));
    }
    return false;
  }
  m->open = true;
  char deviceName[256] = {};
  ma_device_get_name(&m->device, type, deviceName, sizeof(deviceName), nullptr);
  m->name = QString::fromUtf8(deviceName);
  m->updateLatency();
  if (const auto result = ma_device_start(&m->device); result != MA_SUCCESS) {
    ma_device_uninit(&m->device);
    m->open = false;
    if (error) {
      *error = QStringLiteral("cannot start \"%1\": %2").arg(m->name, resultText(result));
    }
    return false;
  }
  m->running = true;
  m->log(QStringLiteral("%1 \"%2\", %3 ms buffer")
             .arg(type == ma_device_type_playback ? QStringLiteral("playing on") : QStringLiteral("recording from"))
             .arg(m->name)
             .arg(m->latency.load(), 0, 'f', 1));
  return true;
}

void AudioDevice::stop()
{
  std::lock_guard lock(m->mutex);
  if (!m->open) {
    return;
  }
  m->running = false;
  ma_device_uninit(&m->device); // waits for the callback to return
  m->open = false;
}

bool AudioDevice::isRunning() const
{
  return m->running;
}

QString AudioDevice::deviceName() const
{
  std::lock_guard lock(m->mutex);
  return m->name;
}

double AudioDevice::latencyMs() const
{
  return m->latency;
}

std::uint64_t AudioDevice::callbacks() const
{
  return m->callbacks.load(std::memory_order_relaxed);
}

QStringList AudioDevice::devices(Direction direction)
{
  QStringList names;
  ma_context context;
  if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
    return names;
  }
  ma_device_info *playback = nullptr;
  ma_device_info *capture = nullptr;
  ma_uint32 playbackCount = 0;
  ma_uint32 captureCount = 0;
  if (ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS) {
    const auto *list = direction == Direction::Playback ? playback : capture;
    const auto count = direction == Direction::Playback ? playbackCount : captureCount;
    for (ma_uint32 i = 0; i < count; ++i) {
      names.append(QString::fromUtf8(list[i].name));
    }
  }
  ma_context_uninit(&context);
  return names;
}

void AudioDevice::Impl::onData(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
  auto *self = static_cast<Impl *>(device->pUserData);
  self->callbacks.fetch_add(1, std::memory_order_relaxed);
  if (self->direction == Direction::Playback) {
    auto *out = static_cast<float *>(output);
    if (self->source) {
      self->source(out, int(frames));
    } else {
      std::fill(out, out + std::size_t(frames) * self->channels, 0.0f);
    }
  } else if (self->sink && input) {
    // The callback comes once a period has been recorded.
    const auto captured = Clock::now() - std::chrono::microseconds(std::int64_t(frames) * 1000000 / kSampleRate);
    self->sink(static_cast<const float *>(input), int(frames), captured);
  }
}

void AudioDevice::Impl::onNotification(const ma_device_notification *notification)
{
  auto *self = static_cast<Impl *>(notification->pDevice->pUserData);
  switch (notification->type) {
  case ma_device_notification_type_stopped:
    if (self->running.exchange(false)) {
      self->log(QStringLiteral("audio device stopped (unplugged or taken away)"));
    }
    break;
  case ma_device_notification_type_rerouted:
    self->updateLatency();
    self->log(QStringLiteral("audio device rerouted to the new default"));
    break;
  default:
    break;
  }
}

void AudioDevice::Impl::updateLatency()
{
  const auto compute = [this](const auto &side) {
    if (side.internalSampleRate > 0) {
      latency = double(side.internalPeriodSizeInFrames) * side.internalPeriods * 1000.0 / side.internalSampleRate;
    }
  };
  if (direction == Direction::Playback) {
    compute(device.playback);
  } else {
    compute(device.capture);
  }
}

} // namespace deskflow::audio
