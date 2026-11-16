/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/ReceiveStream.h"

#include "speex_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace deskflow::audio {

namespace {

constexpr double kMaxPpm = 5000;            // ±0.5% speed: about ±9 cents of pitch
constexpr double kProportionalPpm = 400;    // per ms of buffer error
constexpr double kIntegralPpm = 40;         // per ms·s, absorbs steady clock drift
constexpr double kLevelSmoothingSeconds = 0.5;
constexpr double kTargetDecayMsPerSecond = 4.0;
constexpr std::size_t kTransitWindow = 200; // packets, about 2 s
constexpr int kMaxConcealedRun = 3;         // then fall back to buffering
constexpr int kFadeInFrames = kSampleRate / 400; // 2.5 ms, so a restart does not click
// The speed ratio is (kRatioDenominator + steps) / kRatioDenominator, about
// 15 ppm per step. SpeexDSP 1.2.1 rescales its phase with 32-bit arithmetic
// that overflows for denominators above 2^16; set_rate_frac() then returns
// half way with a stale phase advance and later reads outside its filter
// table. Keeping both terms of the ratio below 2^16 rules that out.
constexpr int kRatioDenominator = 1 << 16;

} // namespace

ReceiveStream::ReceiveStream(const StreamFormat &format) : ReceiveStream(format, Options{})
{
}

ReceiveStream::ReceiveStream(const StreamFormat &format, Options options)
    : m_format(format),
      m_options(options),
      m_decoder(format)
{
  int error = 0;
  m_resampler =
      speex_resampler_init(static_cast<spx_uint32_t>(format.channels), kSampleRate, kSampleRate, options.resamplerQuality, &error);
  m_frame.resize(std::size_t(format.frameSamples) * format.channels);
  m_decoded.reserve(std::size_t(format.frameSamples) * format.channels * 8);
  m_targetMs = m_pullMs + format.frameMs() + options.minTargetMs;
}

ReceiveStream::~ReceiveStream()
{
  if (m_resampler) {
    speex_resampler_destroy(m_resampler);
  }
}

bool ReceiveStream::push(
    const MediaHeader &header, const std::uint8_t *payload, int size, Clock::time_point arrival
)
{
  if (size < 0 || size > kMaxPayloadBytes) {
    m_overflow.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Packet packet;
  packet.sequence = header.sequence;
  packet.timestamp = header.timestamp;
  packet.flags = header.flags;
  packet.size = size;
  packet.arrival = arrival;
  std::memcpy(packet.data.data(), payload, std::size_t(size));
  if (!m_incoming.push(packet)) {
    m_overflow.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void ReceiveStream::pull(float *out, int frames, Clock::time_point now)
{
  const int channels = m_format.channels;
  drainIncoming();

  const double pullMs = frames * 1000.0 / kSampleRate;
  const double elapsed = frames / double(kSampleRate);
  m_pullMs += (pullMs - m_pullMs) * 0.1;
  updateTarget(m_pullMs, elapsed);

  double level = bufferedMs(now);

  if (!m_playing) {
    if (m_haveHighest && level >= m_targetMs) {
      startPlayout(level - m_targetMs);
      level = bufferedMs(now);
    } else {
      m_local.bufferMs = level;
      m_local.havePlayout = false;
      std::fill(out, out + std::size_t(frames) * channels, 0.0f);
      publish();
      return;
    }
  }
  m_local.bufferMs = level;

  updateSpeed(level, elapsed);
  cutExcess(level);

  int produced = 0;
  int guard = 0;
  while (produced < frames) {
    if (decodedFrames() == 0 && !decodeNext()) {
      std::fill(out + std::size_t(produced) * channels, out + std::size_t(frames) * channels, 0.0f);
      ++m_local.underruns;
      stopPlayout();
      break;
    }
    spx_uint32_t inLength = static_cast<spx_uint32_t>(decodedFrames());
    spx_uint32_t outLength = static_cast<spx_uint32_t>(frames - produced);
    speex_resampler_process_interleaved_float(
        m_resampler, m_decoded.data() + m_decodedOffset, &inLength, out + std::size_t(produced) * channels, &outLength
    );
    consumeDecoded(int(inLength));
    produced += int(outLength);
    m_local.inputFrames += inLength;
    m_local.outputFrames += outLength;
    if (++guard > 64) {
      // Never expected; keep the device fed rather than spin.
      std::fill(out + std::size_t(produced) * channels, out + std::size_t(frames) * channels, 0.0f);
      break;
    }
  }
  m_local.havePlayout = m_playing && produced > 0;
  if (m_local.havePlayout) {
    // The resampler holds half its filter of input back; the frames just
    // returned end there.
    const auto latency = std::uint32_t(speex_resampler_get_input_latency(m_resampler));
    m_local.playoutTimestamp = m_decodedEnd - std::uint32_t(decodedFrames()) - latency - std::uint32_t(produced);
    m_local.playoutUs = toMicros(now);
  }
  for (int f = 0; f < produced && m_fadeInDone < kFadeInFrames; ++f, ++m_fadeInDone) {
    const float gain = float(m_fadeInDone) / kFadeInFrames;
    for (int c = 0; c < channels; ++c) {
      out[std::size_t(f) * channels + c] *= gain;
    }
  }
  publish();
}

ReceiveStats ReceiveStream::stats() const
{
  std::lock_guard lock(m_statsMutex);
  return m_published;
}

void ReceiveStream::drainIncoming()
{
  while (auto *packet = m_incoming.front()) {
    store(*packet);
    m_incoming.pop();
  }
}

void ReceiveStream::store(const Packet &packet)
{
  if (m_started && packet.sequence < m_next) {
    ++m_local.late;
    return;
  }
  if (m_haveHighest && packet.sequence + kSlots <= m_highest) {
    ++m_local.late;
    return;
  }
  if (m_haveHighest && packet.sequence > m_highest + kSlots) {
    // A long gap: whatever is still stored is stale.
    m_used.fill(false);
    m_haveHighest = false;
    if (m_playing) {
      stopPlayout();
    }
  }
  const auto index = std::size_t(packet.sequence % kSlots);
  if (m_used[index]) {
    ++m_local.dropped; // duplicate, or an older packet that was never played
    if (m_slots[index].sequence == packet.sequence) {
      return;
    }
  }
  m_slots[index] = packet;
  m_used[index] = true;
  if (!m_haveHighest || packet.sequence > m_highest) {
    m_highest = packet.sequence;
    m_haveHighest = true;
  }
  ++m_local.packets;
  trackArrival(packet);
}

void ReceiveStream::trackArrival(const Packet &packet)
{
  if (!m_haveEpoch) {
    m_epoch = packet.arrival;
    m_lastArrival = packet.arrival;
    m_haveEpoch = true;
  }
  m_lastArrival = std::max(m_lastArrival, packet.arrival);
  if (!m_haveTimestamp) {
    m_lastTimestamp = packet.timestamp;
    m_haveTimestamp = true;
  } else {
    m_unwrappedSamples += double(std::int32_t(packet.timestamp - m_lastTimestamp));
    m_lastTimestamp = packet.timestamp;
  }
  // Arrival time minus send time, up to a constant offset between the clocks.
  const double arrivalMs = std::chrono::duration<double, std::milli>(packet.arrival - m_epoch).count();
  m_transit.push_back(arrivalMs - m_unwrappedSamples * 1000.0 / kSampleRate);
  if (m_transit.size() > kTransitWindow) {
    m_transit.pop_front();
  }
  if (++m_sinceSpread >= 10 || m_transit.size() < 20) {
    m_sinceSpread = 0;
    std::vector<double> sorted(m_transit.begin(), m_transit.end());
    std::sort(sorted.begin(), sorted.end());
    const auto p95 = sorted[std::min(sorted.size() - 1, std::size_t(sorted.size() * 0.95))];
    m_spreadMs = p95 - sorted.front();
  }
}

bool ReceiveStream::decodeNext()
{
  const int channels = m_format.channels;
  const int frameSamples = m_format.frameSamples;
  float *frame = m_frame.data();
  int samples = -1;

  const auto index = std::size_t(m_next % kSlots);
  const bool present = m_used[index] && m_slots[index].sequence == m_next;
  const bool ahead = m_haveHighest && m_highest > m_next;

  std::uint32_t frameTimestamp = m_decodedEnd; // a concealed frame continues the clock
  if (present) {
    auto &packet = m_slots[index];
    frameTimestamp = packet.timestamp;
    if (packet.flags & MediaHeader::Discontinuity) {
      m_decoder.reset();
    }
    samples = m_decoder.decode(packet.data.data(), packet.size, frame, frameSamples);
    m_used[index] = false;
    m_concealedRun = 0;
  } else if (ahead || m_concealedRun < kMaxConcealedRun) {
    samples = m_decoder.conceal(frame, frameSamples);
    ++m_local.concealed;
    if (!ahead) {
      ++m_concealedRun;
    }
  } else {
    return false;
  }

  ++m_next;
  if (samples <= 0) {
    samples = frameSamples;
    std::fill(frame, frame + std::size_t(samples) * channels, 0.0f);
  }
  m_decoded.insert(m_decoded.end(), frame, frame + std::size_t(samples) * channels);
  m_decodedEnd = frameTimestamp + std::uint32_t(samples);
  return true;
}

int ReceiveStream::decodedFrames() const
{
  return int((m_decoded.size() - m_decodedOffset) / std::size_t(m_format.channels));
}

void ReceiveStream::consumeDecoded(int frames)
{
  m_decodedOffset += std::size_t(frames) * m_format.channels;
  if (m_decodedOffset >= m_decoded.size()) {
    m_decoded.clear();
    m_decodedOffset = 0;
  } else if (m_decodedOffset > m_decoded.size() / 2) {
    m_decoded.erase(m_decoded.begin(), m_decoded.begin() + std::ptrdiff_t(m_decodedOffset));
    m_decodedOffset = 0;
  }
}

double ReceiveStream::bufferedMs(Clock::time_point now) const
{
  // Playout drains the buffer a period at a time, but arrivals add a whole
  // packet at once, so a plain count reads low by however long ago the last
  // packet came. Where arrivals fall between pulls slides slowly when the two
  // clocks differ, and the speed controller would chase that slide. Adding
  // the time since the newest arrival (as if packets trickled in at the
  // sending rate) gives a level that is continuous and free of that phase.
  double frames = decodedFrames();
  if (m_haveHighest) {
    std::uint64_t first = m_next;
    if (m_playing || lowestStored(first)) {
      if (m_highest >= first) {
        frames += double(m_highest - first + 1) * m_format.frameSamples;
      }
    }
  }
  double sinceArrivalMs = 0;
  if (m_haveEpoch) {
    sinceArrivalMs = std::clamp(
        std::chrono::duration<double, std::milli>(now - m_lastArrival).count(), 0.0, m_format.frameMs()
    );
  }
  return frames * 1000.0 / kSampleRate + sinceArrivalMs;
}

bool ReceiveStream::lowestStored(std::uint64_t &sequence) const
{
  bool found = false;
  for (int i = 0; i < kSlots; ++i) {
    if (m_used[std::size_t(i)] && (!found || m_slots[std::size_t(i)].sequence < sequence)) {
      sequence = m_slots[std::size_t(i)].sequence;
      found = true;
    }
  }
  return found;
}

void ReceiveStream::startPlayout(double excessMs)
{
  std::uint64_t first = 0;
  if (!lowestStored(first)) {
    return;
  }
  m_next = first;
  m_playing = true;
  m_started = true;
  m_concealedRun = 0;
  m_decoded.clear();
  m_decodedOffset = 0;
  m_haveLevel = false;
  m_fadeInDone = 0;
  speex_resampler_reset_mem(m_resampler);

  // Pulls come a period apart, so buffering overshoots the target by up to a
  // period. Start that much into the audio instead of leaving the speed
  // controller to work it off; it is the very start, so nothing is missed.
  auto trim = std::int64_t(excessMs * kSampleRate / 1000.0);
  while (trim > 0) {
    if (decodedFrames() == 0 && !decodeNext()) {
      break;
    }
    const auto step = std::min<std::int64_t>(trim, decodedFrames());
    consumeDecoded(int(step));
    trim -= step;
  }
}

void ReceiveStream::stopPlayout()
{
  m_playing = false;
  m_decoded.clear();
  m_decodedOffset = 0;
}

void ReceiveStream::updateTarget(double pullMs, double elapsedSeconds)
{
  // The level counts arrivals as continuous, so the data actually present
  // runs up to a packet below it. Allow for that, one device period, the
  // network jitter and a little margin.
  const double floor = pullMs + m_format.frameMs() + m_options.minTargetMs;
  const double wanted = pullMs + m_format.frameMs() + m_spreadMs + m_options.marginMs;
  const double desired = std::clamp(std::max(floor, wanted), floor, std::max(floor, m_options.maxTargetMs));
  if (desired >= m_targetMs) {
    m_targetMs = desired;
  } else {
    m_targetMs = std::max(desired, m_targetMs - kTargetDecayMsPerSecond * elapsedSeconds);
  }
  m_local.targetMs = m_targetMs;
  m_local.jitterMs = m_spreadMs;
}

void ReceiveStream::updateSpeed(double levelMs, double elapsedSeconds)
{
  if (!m_haveLevel) {
    m_levelEma = levelMs;
    m_haveLevel = true;
  }
  const double alpha = std::min(1.0, elapsedSeconds / kLevelSmoothingSeconds);
  m_levelEma += (levelMs - m_levelEma) * alpha;
  const double error = m_levelEma - m_targetMs;
  const double integralLimit = kMaxPpm / kIntegralPpm;
  m_integral = std::clamp(m_integral + error * elapsedSeconds, -integralLimit, integralLimit);
  m_ppm = std::clamp(kProportionalPpm * error + kIntegralPpm * m_integral, -kMaxPpm, kMaxPpm);
  applyRatio(m_ppm);
  m_local.ratioPpm = m_ppm;
}

void ReceiveStream::cutExcess(double levelMs)
{
  const double excess = levelMs - m_targetMs;
  if (excess <= m_options.maxExcessMs) {
    return;
  }
  // Decode and discard whole packets, so the decoder state stays continuous.
  int packets = int((excess - m_format.frameMs()) / m_format.frameMs());
  while (packets-- > 0 && m_haveHighest && m_next < m_highest) {
    const auto before = m_decoded.size();
    if (!decodeNext()) {
      break;
    }
    m_decoded.resize(before);
    ++m_local.dropped;
  }
  m_haveLevel = false;
}

void ReceiveStream::applyRatio(double ppm)
{
  const int steps = int(std::lround(ppm * 1e-6 * kRatioDenominator));
  if (steps == m_appliedSteps) {
    return;
  }
  // Input:output ratio. Above 1 consumes the buffer faster than real time.
  const int result = speex_resampler_set_rate_frac(
      m_resampler, static_cast<spx_uint32_t>(kRatioDenominator + steps), static_cast<spx_uint32_t>(kRatioDenominator),
      kSampleRate, kSampleRate
  );
  if (result != RESAMPLER_ERR_SUCCESS) {
    // Should be impossible with the ratios above; fall back to 1:1 from a clean state.
    speex_resampler_set_rate_frac(m_resampler, 1, 1, kSampleRate, kSampleRate);
    speex_resampler_reset_mem(m_resampler);
    m_appliedSteps = 0;
    ++m_local.resamplerErrors;
    return;
  }
  m_appliedSteps = steps;
}

void ReceiveStream::publish()
{
  m_local.playing = m_playing;
  std::unique_lock lock(m_statsMutex, std::try_to_lock);
  if (lock.owns_lock()) {
    m_published = m_local;
    m_published.dropped += m_overflow.load(std::memory_order_relaxed);
  }
}

} // namespace deskflow::audio
