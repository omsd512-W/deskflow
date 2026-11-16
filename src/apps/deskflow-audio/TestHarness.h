/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioPeer.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

namespace deskflow::audio::test {

//! Feeds a sine wave into the peer's current send stream, paced by a clock
//! that can run fast or slow by clockPpm.
class ToneSource
{
public:
  ToneSource(AudioPeer &peer, StreamKind kind, double hz, double amplitude, double clockPpm);
  ~ToneSource();
  void start();
  void stop();

private:
  void run();

  AudioPeer &m_peer;
  StreamKind m_kind;
  double m_hz;
  double m_amplitude;
  double m_clockPpm;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
};

//! Stands in for a sound card: pulls from the peer's receive stream every
//! period on its own (skewed) clock and checks the tone for glitches.
class VirtualSink
{
public:
  struct Analysis
  {
    std::uint64_t frames = 0;        //!< pulled while a stream existed
    std::uint64_t audibleFrames = 0; //!< non-silent output
    std::uint64_t dropoutFrames = 0; //!< silent output after audio had started
    std::uint64_t glitches = 0;      //!< places where the waveform jumps in a way a clean sine cannot
    double toneHz = 0;               //!< measured over the last stats interval
    double peak = 0;
  };

  VirtualSink(
      AudioPeer &peer, StreamKind kind, double clockPpm, int periodFrames, double expectedHz, double expectedAmplitude
  );
  ~VirtualSink();
  void recordTo(const QString &path, double seconds);
  void start();
  void stop();
  Analysis analysis();

private:
  void run();
  void analyse(const float *samples, int frames, int channels);
  void writeRecording();

  AudioPeer &m_peer;
  StreamKind m_kind;
  double m_clockPpm;
  int m_periodFrames;
  double m_expectedHz;
  double m_expectedAmplitude;
  std::atomic<bool> m_running{false};
  std::thread m_thread;

  std::mutex m_mutex;
  Analysis m_analysis;
  bool m_heard = false;
  float m_previous = 0;
  int m_sinceGlitch = kSampleRate; // samples since the last over-threshold jump
  std::uint64_t m_crossings = 0;
  std::uint64_t m_crossingFrames = 0;

  QString m_recordPath;
  std::size_t m_recordLimit = 0;
  std::vector<float> m_recording;
  int m_recordChannels = 0;
};

//! Delays, reorders and drops datagrams like a bad network would.
class NetworkSimulator
{
public:
  NetworkSimulator(double lossPercent, double delayMs, double jitterMs);
  ~NetworkSimulator();
  SendStream::Transmit wrap(SendStream::Transmit inner);

  std::uint64_t dropped() const
  {
    return m_dropped;
  }

private:
  struct Pending
  {
    Clock::time_point due;
    std::uint64_t order;
    std::vector<std::uint8_t> data;
    SendStream::Transmit *target;
    bool operator>(const Pending &other) const
    {
      return due != other.due ? due > other.due : order > other.order;
    }
  };

  void run();

  double m_loss;
  double m_delayMs;
  double m_jitterMs;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::priority_queue<Pending, std::vector<Pending>, std::greater<>> m_queue;
  std::vector<std::unique_ptr<SendStream::Transmit>> m_targets;
  std::mt19937 m_random{12345};
  std::uint64_t m_order = 0;
  std::atomic<std::uint64_t> m_dropped{0};
  bool m_running = true;
  std::thread m_thread;
};

} // namespace deskflow::audio::test
