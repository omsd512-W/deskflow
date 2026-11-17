/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ReceiveStreamTests.h"

#include "audio/OpusCodec.h"
#include "audio/ReceiveStream.h"

#include <cmath>
#include <numbers>
#include <queue>
#include <random>

using namespace deskflow::audio;

namespace {

// Runs a sender and a receiver on virtual time, so minutes of audio take
// seconds and every run is repeatable.
struct Scenario
{
  StreamKind kind = StreamKind::System;
  double senderPpm = 0;   //!< capture clock error
  double receiverPpm = 0; //!< playback clock error
  double lossPercent = 0;
  double delayMs = 2;
  double jitterMs = 0;    //!< uniform extra delay, reorders packets
  double seconds = 30;
  int periodFrames = 480;
  //! Sender sends nothing in [pauseFrom, pauseTo) seconds.
  double pauseFrom = -1;
  double pauseTo = -1;
  //! Network holds everything sent in [stallFrom, stallTo) until stallTo.
  double stallFrom = -1;
  double stallTo = -1;
  double settleSeconds = 10; //!< excluded from the steady state figures
  double toneHz = 440;
  double blockSeconds = 0; //!< if set, also report the buffer error per block of this length
  unsigned seed = 1;
};

struct Outcome
{
  ReceiveStats stats;
  ReceiveStats settled; //!< stats when the steady state window began
  double meanErrorMs = 0; //!< buffer minus target, steady state
  double meanPpm = 0;     //!< speed correction asked for, steady state
  double effectivePpm = 0; //!< input consumed per output frame, steady state
  double minBufferMs = 1e9;
  int glitches = 0;       //!< steady state
  int audiblePulls = 0;
  int pulls = 0;
  int lost = 0;
  int sent = 0;
  std::vector<double> blockErrorMs;
  std::vector<double> blockPpm;
};

constexpr double kAmplitude = 0.25;

Outcome simulate(const Scenario &s)
{
  const auto format = defaultFormat(s.kind);
  const int channels = format.channels;
  Encoder encoder(format);
  ReceiveStream stream(format);
  std::mt19937 random(s.seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  struct InFlight
  {
    double arrival;
    std::uint64_t order;
    MediaHeader header;
    std::vector<std::uint8_t> payload;
    bool operator>(const InFlight &o) const
    {
      return arrival != o.arrival ? arrival > o.arrival : order > o.order;
    }
  };
  std::priority_queue<InFlight, std::vector<InFlight>, std::greater<>> network;

  const auto epoch = Clock::time_point{} + std::chrono::hours(1);
  const auto at = [&](double seconds) {
    return epoch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  };

  const double sendInterval = format.frameSamples / double(kSampleRate) / (1 + s.senderPpm * 1e-6);
  const double pullInterval = s.periodFrames / double(kSampleRate) / (1 + s.receiverPpm * 1e-6);

  std::vector<float> pcm(std::size_t(format.frameSamples) * channels);
  std::vector<std::uint8_t> encoded(kMaxPayloadBytes);
  std::vector<float> out(std::size_t(s.periodFrames) * channels);
  double phase = 0;
  const double step = 2 * std::numbers::pi * s.toneHz / kSampleRate;
  std::uint64_t sequence = 0;
  std::uint32_t timestamp = 0;
  bool paused = false;
  std::uint64_t order = 0;

  Outcome result;
  double errorSum = 0;
  double ppmSum = 0;
  int steady = 0;
  bool settledTaken = false;
  std::vector<double> blockErrors;
  std::vector<double> blockPpms;
  std::vector<int> blockCounts;
  bool heard = false;
  float previous = 0;
  int sinceGlitch = kSampleRate;
  const double maxStep = 2 * std::numbers::pi * s.toneHz * kAmplitude / kSampleRate * 1.6;

  int sendIndex = 0;
  int pullIndex = 0;
  while (true) {
    const double nextSend = sendIndex * sendInterval;
    const double nextPull = (pullIndex + 1) * pullInterval;
    const double nextArrival = network.empty() ? 1e18 : network.top().arrival;
    if (nextPull > s.seconds) {
      break;
    }
    if (nextSend <= nextPull && nextSend <= nextArrival) {
      // Capture one frame and send it.
      for (int f = 0; f < format.frameSamples; ++f) {
        const float v = float(kAmplitude * std::sin(phase));
        phase += step;
        for (int c = 0; c < channels; ++c) {
          pcm[std::size_t(f) * channels + c] = v;
        }
      }
      phase = std::fmod(phase, 2 * std::numbers::pi);
      const bool inPause = nextSend >= s.pauseFrom && nextSend < s.pauseTo;
      if (inPause) {
        paused = true;
      } else {
        MediaHeader header;
        header.streamId = 1;
        header.sequence = sequence++;
        header.timestamp = timestamp;
        if (paused) {
          header.flags = MediaHeader::Discontinuity;
          encoder.reset();
          paused = false;
        }
        const int size = encoder.encode(pcm.data(), encoded.data(), int(encoded.size()));
        ++result.sent;
        if (unit(random) * 100 < s.lossPercent) {
          ++result.lost;
        } else {
          double arrival = nextSend + (s.delayMs + unit(random) * s.jitterMs) / 1000.0;
          if (nextSend >= s.stallFrom && nextSend < s.stallTo) {
            arrival = std::max(arrival, s.stallTo);
          }
          network.push({arrival, order++, header, std::vector<std::uint8_t>(encoded.begin(), encoded.begin() + size)});
        }
      }
      timestamp += std::uint32_t(format.frameSamples);
      ++sendIndex;
      continue;
    }
    if (nextArrival <= nextPull) {
      auto packet = network.top();
      network.pop();
      stream.push(packet.header, packet.payload.data(), int(packet.payload.size()), at(packet.arrival));
      continue;
    }

    // The playback device asks for the next period.
    stream.pull(out.data(), s.periodFrames, at(nextPull));
    ++pullIndex;
    ++result.pulls;
    const auto stats = stream.stats();
    result.stats = stats;
    const bool inSteadyState = nextPull >= s.settleSeconds;
    if (inSteadyState && !settledTaken) {
      result.settled = stats;
      settledTaken = true;
    }
    bool audible = false;
    for (int f = 0; f < s.periodFrames; ++f) {
      const float v = out[std::size_t(f) * channels];
      audible = audible || v != 0.0f;
      if (sinceGlitch < kSampleRate) {
        ++sinceGlitch;
      }
      if (heard && std::fabs(v - previous) > maxStep) {
        if (sinceGlitch >= kSampleRate / 1000 && inSteadyState) {
          ++result.glitches;
        }
        sinceGlitch = 0;
      }
      previous = v;
    }
    heard = heard || audible;
    if (audible) {
      ++result.audiblePulls;
    }
    if (qEnvironmentVariableIsSet("AUDIO_TRACE") && (result.pulls % 100 == 0 || result.pulls < 12)) {
      qInfo("  t=%6.2f level %6.2f target %6.2f ppm %8.1f in %llu out %llu", nextPull, stats.bufferMs, stats.targetMs,
            stats.ratioPpm, (unsigned long long)stats.inputFrames, (unsigned long long)stats.outputFrames);
    }
    if (s.blockSeconds > 0 && stats.playing) {
      const auto block = std::size_t(nextPull / s.blockSeconds);
      if (blockErrors.size() <= block) {
        blockErrors.resize(block + 1);
        blockPpms.resize(block + 1);
        blockCounts.resize(block + 1);
      }
      blockErrors[block] += stats.bufferMs - stats.targetMs;
      blockPpms[block] += stats.ratioPpm;
      ++blockCounts[block];
    }
    if (inSteadyState && stats.playing) {
      errorSum += stats.bufferMs - stats.targetMs;
      ppmSum += stats.ratioPpm;
      result.minBufferMs = std::min(result.minBufferMs, stats.bufferMs);
      ++steady;
    }
  }
  for (std::size_t i = 0; i < blockCounts.size(); ++i) {
    result.blockErrorMs.push_back(blockCounts[i] ? blockErrors[i] / blockCounts[i] : 0);
    result.blockPpm.push_back(blockCounts[i] ? blockPpms[i] / blockCounts[i] : 0);
  }
  if (steady > 0) {
    result.meanErrorMs = errorSum / steady;
    result.meanPpm = ppmSum / steady;
    const auto in = double(result.stats.inputFrames - result.settled.inputFrames);
    const auto out = double(result.stats.outputFrames - result.settled.outputFrames);
    result.effectivePpm = out > 0 ? (in / out - 1) * 1e6 : 0;
  }
  return result;
}

void report(const char *name, const Outcome &o)
{
  qInfo(
      "%s: buffer error %.2f ms, speed %.0f ppm (effective %.0f), min buffer %.1f ms, target %.1f ms, jitter %.1f ms, "
      "packets %llu, lost %d, concealed %llu, late %llu, dropped %llu, underruns %llu, glitches %d, "
      "resampler errors %llu",
      name, o.meanErrorMs, o.meanPpm, o.effectivePpm, o.minBufferMs, o.stats.targetMs, o.stats.jitterMs,
      (unsigned long long)o.stats.packets, o.lost, (unsigned long long)o.stats.concealed,
      (unsigned long long)o.stats.late, (unsigned long long)o.stats.dropped,
      (unsigned long long)o.stats.underruns, o.glitches, (unsigned long long)o.stats.resamplerErrors
  );
}

} // namespace

void ReceiveStreamTests::test_cleanNetwork()
{
  Scenario s;
  s.seconds = 20;
  s.settleSeconds = 2;
  const auto o = simulate(s);
  report("clean", o);

  QCOMPARE(o.stats.concealed, 0u);
  QCOMPARE(o.stats.late, 0u);
  QCOMPARE(o.stats.underruns, 0u);
  QCOMPARE(o.stats.resamplerErrors, 0u);
  QCOMPARE(o.glitches, 0);
  QVERIFY(std::fabs(o.meanErrorMs) < 1.0);
  QVERIFY(std::fabs(o.meanPpm) < 30);
  // Latency: one period, one packet, the minimum margin; actual delay is half a packet less on average.
  QVERIFY(o.stats.targetMs <= 25.5);
}

void ReceiveStreamTests::test_clockDrift_data()
{
  QTest::addColumn<double>("senderPpm");
  QTest::addColumn<double>("receiverPpm");

  QTest::newRow("playback fast 300") << 0.0 << 300.0;
  QTest::newRow("playback slow 300") << 0.0 << -300.0;
  QTest::newRow("both off 150") << 150.0 << -150.0;
  QTest::newRow("playback fast 3000") << 0.0 << 3000.0;
  QTest::newRow("playback slow 3000") << 0.0 << -3000.0;
}

void ReceiveStreamTests::test_clockDrift()
{
  QFETCH(double, senderPpm);
  QFETCH(double, receiverPpm);

  Scenario s;
  s.senderPpm = senderPpm;
  s.receiverPpm = receiverPpm;
  s.seconds = 120;
  s.settleSeconds = 60;
  const auto o = simulate(s);
  report(QTest::currentDataTag(), o);

  // Consuming (1 + sender) / (1 + receiver) times real time keeps the buffer level.
  const double expectedPpm = ((1 + senderPpm * 1e-6) / (1 + receiverPpm * 1e-6) - 1) * 1e6;
  QVERIFY2(std::fabs(o.effectivePpm - expectedPpm) < 20, qPrintable(QString::number(o.effectivePpm)));
  QVERIFY2(std::fabs(o.meanPpm - expectedPpm) < 20, qPrintable(QString::number(o.meanPpm)));
  QVERIFY2(std::fabs(o.meanErrorMs) < 0.5, qPrintable(QString::number(o.meanErrorMs)));
  // Steady state is clean. Only drift far beyond real hardware (3000 ppm)
  // can outrun the controller while it is still learning the drift.
  QCOMPARE(o.stats.underruns - o.settled.underruns, 0u);
  QCOMPARE(o.stats.concealed - o.settled.concealed, 0u);
  QVERIFY(o.stats.underruns <= (std::fabs(expectedPpm) > 1000 ? 2u : 0u));
  QCOMPARE(o.glitches, 0);
}

void ReceiveStreamTests::test_lossAndJitter_data()
{
  QTest::addColumn<double>("lossPercent");
  QTest::addColumn<double>("jitterMs");

  QTest::newRow("jitter 10 ms") << 0.0 << 10.0;
  QTest::newRow("loss 5%") << 5.0 << 0.0;
  QTest::newRow("loss 5%, jitter 10 ms") << 5.0 << 10.0;
}

void ReceiveStreamTests::test_lossAndJitter()
{
  QFETCH(double, lossPercent);
  QFETCH(double, jitterMs);

  Scenario s;
  s.lossPercent = lossPercent;
  s.jitterMs = jitterMs;
  s.receiverPpm = 200;
  s.seconds = 90;
  s.settleSeconds = 10;
  const auto o = simulate(s);
  report(QTest::currentDataTag(), o);

  const auto lateAfterSettling = o.stats.late - o.settled.late;
  const auto underrunsAfterSettling = o.stats.underruns - o.settled.underruns;
  // Every lost packet is concealed; packets that are merely late are rare.
  QVERIFY(o.stats.concealed >= std::uint64_t(o.lost) * 9 / 10);
  QVERIFY2(lateAfterSettling <= 2, qPrintable(QString::number(lateAfterSettling)));
  QCOMPARE(underrunsAfterSettling, 0u);
  // The buffer covers the jitter, and not much more.
  QVERIFY(o.stats.targetMs >= jitterMs);
  // Period + packet + margin + jitter, give or take the estimator's slack.
  QVERIFY2(o.stats.targetMs <= 10 + 10 + 5 + jitterMs + 3, qPrintable(QString::number(o.stats.targetMs)));
  QVERIFY(std::fabs(o.meanErrorMs) < 2.0);
}

void ReceiveStreamTests::test_microphoneLoss_data()
{
  QTest::addColumn<double>("toneHz");
  // SILK left a transient after every loss at some of these; CELT must not.
  QTest::newRow("200 Hz") << 200.0;
  QTest::newRow("1 kHz") << 1000.0;
  QTest::newRow("3 kHz") << 3000.0;
}

void ReceiveStreamTests::test_microphoneLoss()
{
  QFETCH(double, toneHz);
  Scenario s;
  s.kind = StreamKind::Microphone;
  s.toneHz = toneHz;
  s.lossPercent = 5;
  s.jitterMs = 5;
  s.seconds = 60;
  const auto o = simulate(s);
  report(QTest::currentDataTag(), o);

  QVERIFY(o.stats.concealed >= std::uint64_t(o.lost) * 9 / 10);
  QCOMPARE(o.stats.underruns - o.settled.underruns, 0u);
  QCOMPARE(o.glitches, 0);
}

void ReceiveStreamTests::test_longRun()
{
  // Minutes of virtual time per real second; set AUDIO_LONG_SECONDS=3600 for the hour.
  const int seconds = qEnvironmentVariableIntValue("AUDIO_LONG_SECONDS");
  if (seconds <= 0) {
    QSKIP("set AUDIO_LONG_SECONDS to run");
  }
  Scenario s;
  s.lossPercent = qEnvironmentVariableIsSet("AUDIO_LONG_LOSS") ? qEnvironmentVariable("AUDIO_LONG_LOSS").toDouble() : 5;
  s.jitterMs = 10;
  s.receiverPpm = 200;
  s.seconds = seconds;
  s.settleSeconds = 60;
  s.blockSeconds = 600;
  const auto o = simulate(s);
  report("long run", o);
  for (std::size_t i = 0; i < o.blockErrorMs.size(); ++i) {
    qInfo("  minutes %3d-%3d: buffer error %+.3f ms, speed %.1f ppm", int(i * 10), int(i * 10 + 10), o.blockErrorMs[i],
          o.blockPpm[i]);
  }
  // Without loss there is never an underrun. With 5% random loss, four or
  // more packets in a row (40 ms) go missing about twice an hour, which no
  // 30 ms buffer can bridge; anything beyond that would point to drift.
  const auto underruns = o.stats.underruns - o.settled.underruns;
  QVERIFY2(underruns <= (s.lossPercent > 0 ? 3u : 0u), qPrintable(QString::number(underruns)));
  // The point of the hour: the level does not creep.
  for (std::size_t i = 1; i < o.blockErrorMs.size(); ++i) {
    QVERIFY(std::fabs(o.blockErrorMs[i]) < 0.5);
  }
}

void ReceiveStreamTests::test_pauseAndResume()
{
  Scenario s;
  s.seconds = 20;
  s.settleSeconds = 1;
  s.pauseFrom = 5;
  s.pauseTo = 8;
  const auto o = simulate(s);
  report("pause", o);

  // The gap ends playout once; afterwards it rebuffers and plays again.
  QCOMPARE(o.stats.underruns, 1u);
  QVERIFY(o.stats.concealed <= 3);
  QVERIFY(o.stats.playing);
  QVERIFY(o.audiblePulls > int(s.seconds - 3 - 1) * 100 - 10);
}

void ReceiveStreamTests::test_burstIsCut()
{
  Scenario s;
  s.seconds = 20;
  s.settleSeconds = 1;
  s.stallFrom = 5;
  s.stallTo = 5.3; // 300 ms of audio arrives at once
  const auto o = simulate(s);
  report("burst", o);

  // The stall itself plays out as concealment and an underrun...
  QVERIFY(o.stats.underruns >= 1);
  // ...and the backlog it leaves is dropped instead of adding 300 ms of delay.
  QVERIFY(o.stats.dropped > 0 || o.stats.late > 0);
  QVERIFY(std::fabs(o.stats.bufferMs - o.stats.targetMs) < 15);
}

void ReceiveStreamTests::test_oddDevicePeriod()
{
  Scenario s;
  s.periodFrames = 512; // a power of two, as many devices use
  s.receiverPpm = -100;
  s.seconds = 60;
  s.settleSeconds = 20;
  const auto o = simulate(s);
  report("period 512", o);

  QCOMPARE(o.stats.underruns, 0u);
  QCOMPARE(o.stats.concealed, 0u);
  QCOMPARE(o.glitches, 0);
  QVERIFY(std::fabs(o.meanErrorMs) < 1.0);
}

QTEST_MAIN(ReceiveStreamTests)
