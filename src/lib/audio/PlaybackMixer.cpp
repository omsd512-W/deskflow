/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/PlaybackMixer.h"

#include <algorithm>

namespace deskflow::audio {

PlaybackMixer::PlaybackMixer(int channels, int sampleRate)
    : m_channels(channels),
      m_fadeFrames(std::max(1, sampleRate * kFadeMs / 1000)),
      m_state(std::make_shared<const State>())
{
}

std::shared_ptr<const PlaybackMixer::State> PlaybackMixer::state() const
{
  std::lock_guard lock(m_mutex);
  return m_state;
}

void PlaybackMixer::update(const std::function<void(State &)> &change)
{
  std::shared_ptr<const State> old;
  std::lock_guard lock(m_mutex);
  auto next = std::make_shared<State>(*m_state);
  change(*next);
  old.swap(m_state); // released after the lock, outside the output thread's way
  m_state = std::move(next);
}

void PlaybackMixer::setSources(std::vector<Source> sources)
{
  update([&sources](State &state) { state.sources = std::move(sources); });
}

void PlaybackMixer::setMode(Mode mode)
{
  update([mode](State &state) { state.mode = mode; });
}

void PlaybackMixer::setActive(const QString &name)
{
  update([&name](State &state) { state.active = name; });
}

bool PlaybackMixer::hasSources() const
{
  return !state()->sources.empty();
}

PlaybackMixer::Mode PlaybackMixer::mode() const
{
  return state()->mode;
}

QString PlaybackMixer::active() const
{
  return state()->active;
}

bool PlaybackMixer::isAudible(const QString &name) const
{
  return target(*state(), name) > 0;
}

float PlaybackMixer::target(const State &state, const QString &name)
{
  if (state.mode == Mode::Mix || state.active.isEmpty()) {
    return 1.0f;
  }
  // Deskflow's screen names ignore case.
  return name.compare(state.active, Qt::CaseInsensitive) == 0 ? 1.0f : 0.0f;
}

void PlaybackMixer::pull(float *out, int frames)
{
  const auto samples = std::size_t(frames) * std::size_t(m_channels);
  std::fill(out, out + samples, 0.0f);
  const auto current = state();
  if (m_scratch.size() < samples) {
    m_scratch.resize(samples);
  }
  const float step = 1.0f / float(m_fadeFrames);
  for (const auto &source : current->sources) {
    source.pull(m_scratch.data(), frames);
    const float goal = target(*current, source.name);
    auto it = m_gains.find(source.name);
    if (it == m_gains.end()) {
      it = m_gains.emplace(source.name, goal).first; // a new source starts where it should be
    }
    float gain = it->second;
    if (gain == goal) {
      if (gain > 0) {
        for (std::size_t i = 0; i < samples; ++i) {
          out[i] += gain * m_scratch[i];
        }
      }
      continue;
    }
    for (int frame = 0; frame < frames; ++frame) {
      gain = goal > gain ? std::min(goal, gain + step) : std::max(goal, gain - step);
      const auto base = std::size_t(frame) * std::size_t(m_channels);
      for (int channel = 0; channel < m_channels; ++channel) {
        out[base + channel] += gain * m_scratch[base + channel];
      }
    }
    it->second = gain;
  }
  for (std::size_t i = 0; i < samples; ++i) {
    out[i] = std::clamp(out[i], -1.0f, 1.0f);
  }
  // Forget sources that are gone, so one that comes back starts afresh.
  if (m_gains.size() > current->sources.size()) {
    for (auto it = m_gains.begin(); it != m_gains.end();) {
      const bool present = std::any_of(current->sources.begin(), current->sources.end(), [&it](const Source &source) {
        return source.name == it->first;
      });
      it = present ? std::next(it) : m_gains.erase(it);
    }
  }
}

} // namespace deskflow::audio
