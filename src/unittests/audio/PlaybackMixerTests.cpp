/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PlaybackMixerTests.h"

#include "audio/PlaybackMixer.h"

#include <vector>

using namespace deskflow::audio;

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;

//! A source that plays a constant level and counts how often it was pulled.
PlaybackMixer::Source constant(const QString &name, float level, int *pulls = nullptr)
{
  return {name, [level, pulls](float *out, int frames) {
            std::fill(out, out + std::size_t(frames) * 2, level);
            if (pulls) {
              ++*pulls;
            }
          }};
}

std::vector<float> pull(PlaybackMixer &mixer, int frames = kFrames)
{
  std::vector<float> out(std::size_t(frames) * 2, -9.0f);
  mixer.pull(out.data(), frames);
  return out;
}

} // namespace

void PlaybackMixerTests::test_mixesEveryone()
{
  PlaybackMixer mixer(2, kRate);
  QVERIFY(!mixer.hasSources());
  QCOMPARE(pull(mixer).front(), 0.0f); // nothing to play is silence
  mixer.setSources({constant("mini", 0.25f), constant("studio", 0.5f)});
  QVERIFY(mixer.hasSources());
  const auto out = pull(mixer);
  QCOMPARE(out.front(), 0.75f);
  QCOMPARE(out.back(), 0.75f);
  QVERIFY(mixer.isAudible("mini") && mixer.isAudible("studio"));
}

void PlaybackMixerTests::test_clampsLoudMix()
{
  PlaybackMixer mixer(2, kRate);
  mixer.setSources({constant("mini", 0.8f), constant("studio", 0.8f)});
  QCOMPARE(pull(mixer).front(), 1.0f);
  mixer.setSources({constant("mini", -0.8f), constant("studio", -0.8f)});
  QCOMPARE(pull(mixer).front(), -1.0f);
}

void PlaybackMixerTests::test_followsCursor()
{
  PlaybackMixer mixer(2, kRate);
  mixer.setMode(PlaybackMixer::Mode::FollowCursor);
  mixer.setActive("Studio"); // screen names ignore case
  mixer.setSources({constant("mini", 0.25f), constant("studio", 0.5f)});
  QCOMPARE(pull(mixer).front(), 0.5f);
  QVERIFY(!mixer.isAudible("mini"));
  QVERIFY(mixer.isAudible("studio"));

  // The cursor on the server itself: no client is heard.
  mixer.setActive("windows-pc");
  pull(mixer, 48000 * PlaybackMixer::kFadeMs / 1000);
  QCOMPARE(pull(mixer).front(), 0.0f);

  // Back to mixing.
  mixer.setMode(PlaybackMixer::Mode::Mix);
  pull(mixer, 48000 * PlaybackMixer::kFadeMs / 1000);
  QCOMPARE(pull(mixer).front(), 0.75f);
}

void PlaybackMixerTests::test_fadesWhenCursorMoves()
{
  PlaybackMixer mixer(2, kRate);
  mixer.setMode(PlaybackMixer::Mode::FollowCursor);
  mixer.setActive("mini");
  mixer.setSources({constant("mini", 0.5f), constant("studio", 0.5f)});
  QCOMPARE(pull(mixer).front(), 0.5f);

  mixer.setActive("studio");
  const auto fadeFrames = kRate * PlaybackMixer::kFadeMs / 1000;
  const auto out = pull(mixer, fadeFrames);
  // One fades out as the other fades in: no gap and no jump.
  for (int frame = 0; frame < fadeFrames; ++frame) {
    QVERIFY2(std::abs(out[std::size_t(frame) * 2] - 0.5f) < 1e-4f, qPrintable(QString::number(frame)));
  }
  // A lone source fades in rather than starting at full level.
  mixer.setActive("mini");
  pull(mixer, fadeFrames);
  mixer.setSources({constant("studio", 0.5f)});
  mixer.setActive("studio");
  const auto fadeIn = pull(mixer, fadeFrames);
  QVERIFY(fadeIn.front() < 0.01f);
  QVERIFY(fadeIn[std::size_t(fadeFrames / 2) * 2] > 0.2f && fadeIn[std::size_t(fadeFrames / 2) * 2] < 0.3f);
  QCOMPARE(pull(mixer).front(), 0.5f);
}

void PlaybackMixerTests::test_unknownComputerPlaysEveryone()
{
  PlaybackMixer mixer(2, kRate);
  mixer.setMode(PlaybackMixer::Mode::FollowCursor);
  mixer.setSources({constant("mini", 0.25f), constant("studio", 0.5f)});
  QCOMPARE(pull(mixer).front(), 0.75f);
  QVERIFY(mixer.isAudible("mini"));
}

void PlaybackMixerTests::test_silentSourceIsStillPulled()
{
  PlaybackMixer mixer(2, kRate);
  mixer.setMode(PlaybackMixer::Mode::FollowCursor);
  mixer.setActive("mini");
  int miniPulls = 0;
  int studioPulls = 0;
  mixer.setSources({constant("mini", 0.25f, &miniPulls), constant("studio", 0.5f, &studioPulls)});
  for (int i = 0; i < 5; ++i) {
    QCOMPARE(pull(mixer).front(), 0.25f);
  }
  // Its jitter buffer keeps draining, so it comes in on time when the cursor arrives.
  QCOMPARE(miniPulls, 5);
  QCOMPARE(studioPulls, 5);
}

QTEST_MAIN(PlaybackMixerTests)
