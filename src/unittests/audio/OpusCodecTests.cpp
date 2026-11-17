/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OpusCodecTests.h"

#include "audio/AudioTypes.h"
#include "audio/OpusCodec.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace deskflow::audio;

Q_DECLARE_METATYPE(deskflow::audio::StreamKind)

namespace {

constexpr double kAmplitude = 0.25;

//! Fills one frame of a sine, continuing the phase.
void tone(const StreamFormat &format, double hz, double &phase, std::vector<float> &pcm)
{
  pcm.resize(std::size_t(format.frameSamples) * format.channels);
  const double step = 2 * std::numbers::pi * hz / kSampleRate;
  for (int f = 0; f < format.frameSamples; ++f) {
    for (int c = 0; c < format.channels; ++c) {
      pcm[std::size_t(f) * format.channels + c] = float(kAmplitude * std::sin(phase));
    }
    phase += step;
  }
}

double rms(const std::vector<float> &samples)
{
  double sum = 0;
  for (float s : samples) {
    sum += double(s) * s;
  }
  return std::sqrt(sum / double(samples.size()));
}

} // namespace

void OpusCodecTests::test_roundTrip_data()
{
  QTest::addColumn<StreamKind>("kind");
  QTest::addColumn<int>("maxBytes");
  // 192 kbit/s and 64 kbit/s at 10 ms are 240 and 80 bytes; VBR may go a little over.
  QTest::newRow("system") << StreamKind::System << 400;
  QTest::newRow("microphone") << StreamKind::Microphone << 200;
}

void OpusCodecTests::test_roundTrip()
{
  QFETCH(StreamKind, kind);
  QFETCH(int, maxBytes);
  const auto format = defaultFormat(kind);
  Encoder encoder(format);
  Decoder decoder(format);
  QVERIFY(encoder.isValid());
  QVERIFY(decoder.isValid());

  double phase = 0;
  std::vector<float> pcm;
  std::vector<float> decoded(std::size_t(format.frameSamples) * format.channels);
  std::vector<float> all;
  std::vector<std::uint8_t> packet(kMaxPayloadBytes);
  int largest = 0;
  for (int i = 0; i < 100; ++i) {
    tone(format, 1000, phase, pcm);
    const int size = encoder.encode(pcm.data(), packet.data(), int(packet.size()));
    QVERIFY(size > 0);
    largest = std::max(largest, size);
    QCOMPARE(decoder.decode(packet.data(), size, decoded.data(), format.frameSamples), format.frameSamples);
    if (i >= 10) {
      all.insert(all.end(), decoded.begin(), decoded.end());
    }
  }
  QVERIFY2(largest <= maxBytes, qPrintable(QString::number(largest)));

  // Level within 1 dB, and still a 1 kHz tone (zero crossings).
  const double expected = kAmplitude / std::sqrt(2.0);
  QVERIFY(std::fabs(20 * std::log10(rms(all) / expected)) < 1.0);
  int crossings = 0;
  for (std::size_t i = format.channels; i < all.size(); i += format.channels) {
    crossings += all[i - format.channels] < 0 && all[i] >= 0;
  }
  const double hz = crossings * double(kSampleRate) / double(all.size() / format.channels);
  QVERIFY2(std::fabs(hz - 1000) < 5, qPrintable(QString::number(hz)));
}

void OpusCodecTests::test_concealment_data()
{
  QTest::addColumn<StreamKind>("kind");
  QTest::newRow("system") << StreamKind::System;
  QTest::newRow("microphone") << StreamKind::Microphone;
}

void OpusCodecTests::test_concealment()
{
  QFETCH(StreamKind, kind);
  const auto format = defaultFormat(kind);
  Encoder encoder(format);
  Decoder decoder(format);
  double phase = 0;
  std::vector<float> pcm;
  std::vector<float> decoded(std::size_t(format.frameSamples) * format.channels);
  std::vector<std::uint8_t> packet(kMaxPayloadBytes);
  for (int i = 0; i < 20; ++i) {
    tone(format, 440, phase, pcm);
    const int size = encoder.encode(pcm.data(), packet.data(), int(packet.size()));
    decoder.decode(packet.data(), size, decoded.data(), format.frameSamples);
  }
  // A missing packet is synthesized, and the tone carries on rather than
  // stopping dead.
  QCOMPARE(decoder.conceal(decoded.data(), format.frameSamples), format.frameSamples);
  QVERIFY(rms(decoded) > kAmplitude / std::sqrt(2.0) * 0.3);
  for (float s : decoded) {
    QVERIFY(std::isfinite(s));
    QVERIFY(std::fabs(s) <= 1.0f);
  }
}

QTEST_MAIN(OpusCodecTests)
