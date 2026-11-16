/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TestHarness.h"

#include <QFile>

#include <cmath>
#include <numbers>

namespace deskflow::audio::test {

namespace {

Clock::duration scaledPeriod(int frames, double clockPpm)
{
  const double seconds = frames / double(kSampleRate) * (1.0 + clockPpm * 1e-6);
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

} // namespace

ToneSource::ToneSource(AudioPeer &peer, StreamKind kind, double hz, double amplitude, double clockPpm)
    : m_peer(peer),
      m_kind(kind),
      m_hz(hz),
      m_amplitude(amplitude),
      m_clockPpm(clockPpm)
{
}

ToneSource::~ToneSource()
{
  stop();
}

void ToneSource::start()
{
  m_running = true;
  m_thread = std::thread([this] { run(); });
}

void ToneSource::stop()
{
  m_running = false;
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void ToneSource::run()
{
  const auto format = defaultFormat(m_kind);
  const int frames = format.frameSamples;
  std::vector<float> buffer(std::size_t(frames) * format.channels);
  const double step = 2 * std::numbers::pi * m_hz / kSampleRate;
  double phase = 0;
  auto next = Clock::now();
  const auto period = scaledPeriod(frames, -m_clockPpm); // a fast clock has a shorter period
  while (m_running) {
    for (int f = 0; f < frames; ++f) {
      const float v = float(m_amplitude * std::sin(phase));
      phase += step;
      for (int c = 0; c < format.channels; ++c) {
        buffer[std::size_t(f) * format.channels + c] = v;
      }
    }
    phase = std::fmod(phase, 2 * std::numbers::pi);
    if (auto stream = m_peer.sendStream(m_kind)) {
      stream->push(buffer.data(), frames);
    }
    next += period;
    std::this_thread::sleep_until(next);
  }
}

VirtualSink::VirtualSink(
    AudioPeer &peer, StreamKind kind, double clockPpm, int periodFrames, double expectedHz, double expectedAmplitude
)
    : m_peer(peer),
      m_kind(kind),
      m_clockPpm(clockPpm),
      m_periodFrames(periodFrames),
      m_expectedHz(expectedHz),
      m_expectedAmplitude(expectedAmplitude)
{
}

VirtualSink::~VirtualSink()
{
  stop();
}

void VirtualSink::recordTo(const QString &path, double seconds)
{
  m_recordPath = path;
  m_recordLimit = std::size_t(seconds * kSampleRate);
}

void VirtualSink::start()
{
  m_running = true;
  m_thread = std::thread([this] { run(); });
}

void VirtualSink::stop()
{
  m_running = false;
  if (m_thread.joinable()) {
    m_thread.join();
    writeRecording();
  }
}

VirtualSink::Analysis VirtualSink::analysis()
{
  std::lock_guard lock(m_mutex);
  auto result = m_analysis;
  result.toneHz = m_crossingFrames ? m_crossings * double(kSampleRate) / double(m_crossingFrames) : 0;
  m_crossings = 0;
  m_crossingFrames = 0;
  return result;
}

void VirtualSink::run()
{
  std::vector<float> buffer;
  auto next = Clock::now();
  const auto period = scaledPeriod(m_periodFrames, -m_clockPpm);
  while (m_running) {
    if (auto stream = m_peer.receiveStream(m_kind)) {
      const int channels = stream->format().channels;
      buffer.resize(std::size_t(m_periodFrames) * channels);
      stream->pull(buffer.data(), m_periodFrames);
      analyse(buffer.data(), m_periodFrames, channels);
      if (m_recordLimit > 0 && m_recording.size() < m_recordLimit * channels) {
        m_recordChannels = channels;
        m_recording.insert(m_recording.end(), buffer.begin(), buffer.end());
      }
    } else {
      std::lock_guard lock(m_mutex);
      m_heard = false;
    }
    next += period;
    std::this_thread::sleep_until(next);
  }
}

void VirtualSink::analyse(const float *samples, int frames, int channels)
{
  // A sine of amplitude A changes by at most 2*pi*f*A/rate per sample; the
  // margin absorbs codec noise. Jumps closer than 1 ms count as one glitch.
  const double maxStep = 2 * std::numbers::pi * m_expectedHz * m_expectedAmplitude / kSampleRate * 1.6;
  constexpr int kGlitchGap = kSampleRate / 1000;
  std::lock_guard lock(m_mutex);
  m_analysis.frames += std::uint64_t(frames);
  bool audible = false;
  for (int f = 0; f < frames; ++f) {
    const float v = samples[std::size_t(f) * channels];
    m_analysis.peak = std::max(m_analysis.peak, double(std::fabs(v)));
    if (v != 0.0f) {
      audible = true;
    }
    if (m_sinceGlitch < kGlitchGap) {
      ++m_sinceGlitch;
    }
    if (m_heard && std::fabs(v - m_previous) > maxStep) {
      if (m_sinceGlitch >= kGlitchGap) {
        ++m_analysis.glitches;
      }
      m_sinceGlitch = 0;
    }
    if (m_previous < 0 && v >= 0) {
      ++m_crossings;
    }
    m_previous = v;
  }
  m_crossingFrames += std::uint64_t(frames);
  if (audible) {
    m_analysis.audibleFrames += std::uint64_t(frames);
    m_heard = true;
  } else if (m_heard) {
    m_analysis.dropoutFrames += std::uint64_t(frames);
  }
}

void VirtualSink::writeRecording()
{
  if (m_recordPath.isEmpty() || m_recording.empty()) {
    return;
  }
  QFile file(m_recordPath);
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  const auto bytes = quint32(m_recording.size() * sizeof(float));
  const auto u32 = [&file](quint32 v) { file.write(reinterpret_cast<const char *>(&v), 4); };
  const auto u16 = [&file](quint16 v) { file.write(reinterpret_cast<const char *>(&v), 2); };
  file.write("RIFF", 4);
  u32(36 + bytes);
  file.write("WAVEfmt ", 8);
  u32(16);
  u16(3);
  u16(quint16(m_recordChannels));
  u32(kSampleRate);
  u32(kSampleRate * m_recordChannels * 4);
  u16(quint16(m_recordChannels * 4));
  u16(32);
  file.write("data", 4);
  u32(bytes);
  file.write(reinterpret_cast<const char *>(m_recording.data()), qint64(bytes));
}

NetworkSimulator::NetworkSimulator(double lossPercent, double delayMs, double jitterMs)
    : m_loss(lossPercent / 100.0),
      m_delayMs(delayMs),
      m_jitterMs(jitterMs)
{
  m_thread = std::thread([this] { run(); });
}

NetworkSimulator::~NetworkSimulator()
{
  {
    std::lock_guard lock(m_mutex);
    m_running = false;
  }
  m_wake.notify_all();
  m_thread.join();
}

SendStream::Transmit NetworkSimulator::wrap(SendStream::Transmit inner)
{
  std::lock_guard lock(m_mutex);
  m_targets.push_back(std::make_unique<SendStream::Transmit>(std::move(inner)));
  auto *target = m_targets.back().get();
  return [this, target](const std::uint8_t *data, int size) {
    std::lock_guard lock(m_mutex);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    if (unit(m_random) < m_loss) {
      ++m_dropped;
      return;
    }
    const double delay = m_delayMs + unit(m_random) * m_jitterMs;
    Pending pending;
    pending.due = Clock::now() +
                  std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(delay));
    pending.order = m_order++;
    pending.data.assign(data, data + size);
    pending.target = target;
    m_queue.push(std::move(pending));
    m_wake.notify_one();
  };
}

void NetworkSimulator::run()
{
  std::unique_lock lock(m_mutex);
  while (m_running) {
    if (m_queue.empty()) {
      m_wake.wait(lock);
      continue;
    }
    const auto due = m_queue.top().due;
    if (Clock::now() < due) {
      m_wake.wait_until(lock, due);
      continue;
    }
    auto pending = m_queue.top();
    m_queue.pop();
    lock.unlock();
    (*pending.target)(pending.data.data(), int(pending.data.size()));
    lock.lock();
  }
}

} // namespace deskflow::audio::test
