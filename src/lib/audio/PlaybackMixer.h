/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace deskflow::audio {

/**
 * Plays the sound of several computers on one output device: either all of
 * them mixed together, or only the one the cursor is on.
 *
 * Sources and settings change on the main thread; pull() runs on the output
 * device's thread. A source that should not be heard is still pulled, so its
 * jitter buffer keeps up and it can come in right away, and it fades in and
 * out instead of clicking.
 */
class PlaybackMixer
{
public:
  enum class Mode
  {
    Mix,          //!< everyone at once
    FollowCursor, //!< only the computer the cursor is on
  };

  struct Source
  {
    QString name; //!< the computer's name, as the cursor reports it
    //! Fills `frames` interleaved frames with the next of its audio.
    std::function<void(float *out, int frames)> pull;
  };

  static constexpr int kFadeMs = 20;

  PlaybackMixer(int channels, int sampleRate);

  void setSources(std::vector<Source> sources);
  void setMode(Mode mode);
  //! The computer the cursor is on; empty while unknown, which plays everyone.
  void setActive(const QString &name);

  bool hasSources() const;
  Mode mode() const;
  QString active() const;
  //! Whether this source is meant to be heard now (the fade may still be running).
  bool isAudible(const QString &name) const;

  //! Output-thread side: mix the next `frames` interleaved frames into `out`.
  void pull(float *out, int frames);

private:
  struct State
  {
    std::vector<Source> sources;
    Mode mode = Mode::Mix;
    QString active;
  };

  std::shared_ptr<const State> state() const;
  void update(const std::function<void(State &)> &change);
  static float target(const State &state, const QString &name);

  const int m_channels;
  const int m_fadeFrames;
  mutable std::mutex m_mutex;
  std::shared_ptr<const State> m_state;

  // Output thread only.
  std::vector<float> m_scratch;
  std::map<QString, float> m_gains;
};

} // namespace deskflow::audio
