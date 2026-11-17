/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"
#include "audio/MediaPacket.h"
#include "audio/OpusCodec.h"
#include "audio/SpscQueue.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct SpeexResamplerState_;

namespace deskflow::audio {

struct ReceiveStats
{
  std::uint64_t packets = 0;      //!< accepted from the network
  std::uint64_t concealed = 0;    //!< frames synthesized for packets that were missing
  std::uint64_t late = 0;         //!< arrived after their playout time
  std::uint64_t dropped = 0;      //!< discarded: duplicates, queue overflow, or cut to reduce delay
  std::uint64_t underruns = 0;    //!< times playout ran dry and went back to buffering
  std::uint64_t resamplerErrors = 0;
  std::uint64_t inputFrames = 0;  //!< decoded frames fed through the resampler
  std::uint64_t outputFrames = 0; //!< frames played (excluding silence while buffering)
  //! Sender sample clock of the first frame the last pull returned, and when
  //! that pull happened (toMicros). Only valid while havePlayout.
  bool havePlayout = false;
  std::uint32_t playoutTimestamp = 0;
  std::int64_t playoutUs = 0;
  double bufferMs = 0;            //!< audio ahead of playout before the last pull (see bufferedMs)
  double targetMs = 0;            //!< buffer the controller is steering towards
  double jitterMs = 0;            //!< 95th percentile spread of arrival delay
  double ratioPpm = 0;            //!< current playback speed correction
  bool playing = false;
};

/**
 * Receive side of one stream: jitter buffer, Opus decoder, loss concealment
 * and clock drift compensation.
 *
 * push() is called by the network thread and pull() by the audio device
 * thread; they only share a lock-free queue. The device clock and the
 * sender's clock never agree exactly, so the playback speed is nudged by up
 * to ±0.5% (resampling) to hold the buffer at a target that follows the
 * measured network jitter.
 */
class ReceiveStream
{
public:
  struct Options
  {
    double minTargetMs = 5; //!< added on top of one device period and one packet
    double maxTargetMs = 150;
    double marginMs = 3;
    //! Cut whole packets when the buffer is this far above target.
    double maxExcessMs = 60;
    int resamplerQuality = 5;
  };

  explicit ReceiveStream(const StreamFormat &format);
  ReceiveStream(const StreamFormat &format, Options options);
  ~ReceiveStream();
  ReceiveStream(const ReceiveStream &) = delete;
  ReceiveStream &operator=(const ReceiveStream &) = delete;

  const StreamFormat &format() const
  {
    return m_format;
  }

  //! Network thread. @return false if the queue was full and the packet dropped.
  bool push(const MediaHeader &header, const std::uint8_t *payload, int size, Clock::time_point arrival);

  //! Audio thread. Fills frames * channels interleaved samples (silence while buffering).
  void pull(float *out, int frames)
  {
    pull(out, frames, Clock::now());
  }
  //! As above, at a given time (tests run on a virtual clock).
  void pull(float *out, int frames, Clock::time_point now);

  //! Any thread.
  ReceiveStats stats() const;

private:
  struct Packet
  {
    std::uint64_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint8_t flags = 0;
    int size = 0;
    Clock::time_point arrival;
    std::array<std::uint8_t, kMaxPayloadBytes> data{};
  };

  static constexpr int kSlots = 64;

  void drainIncoming();
  void store(const Packet &packet);
  void trackArrival(const Packet &packet);
  bool decodeNext();
  int decodedFrames() const;
  void consumeDecoded(int frames);
  double bufferedMs(Clock::time_point now) const;
  bool lowestStored(std::uint64_t &sequence) const;
  void startPlayout(double excessMs);
  void stopPlayout();
  void updateTarget(double pullMs, double elapsedSeconds);
  void updateSpeed(double levelMs, double elapsedSeconds);
  void cutExcess(double levelMs);
  void applyRatio(double ppm);
  void publish();

  StreamFormat m_format;
  Options m_options;
  SpscQueue<Packet, 128> m_incoming;
  std::atomic<std::uint64_t> m_overflow{0};

  // Everything below belongs to the audio thread.
  Decoder m_decoder;
  SpeexResamplerState_ *m_resampler = nullptr;
  std::array<Packet, kSlots> m_slots{};
  std::array<bool, kSlots> m_used{};
  bool m_haveHighest = false;
  std::uint64_t m_highest = 0;
  bool m_playing = false;
  bool m_started = false; //!< playout began at least once, so m_next is meaningful
  std::uint64_t m_next = 0;
  int m_concealedRun = 0;
  int m_fadeInDone = 0; //!< frames of the fade-in already played since playout began
  std::uint32_t m_decodedEnd = 0; //!< sender sample clock just past the last decoded frame
  std::vector<float> m_decoded;
  std::size_t m_decodedOffset = 0;
  std::vector<float> m_frame;

  bool m_haveTimestamp = false;
  std::uint32_t m_lastTimestamp = 0;
  double m_unwrappedSamples = 0;
  Clock::time_point m_epoch;
  bool m_haveEpoch = false;
  Clock::time_point m_lastArrival;
  std::deque<double> m_transit;
  int m_sinceSpread = 0;
  double m_spreadMs = 0;

  double m_pullMs = 10;
  double m_targetMs = 0;
  bool m_haveLevel = false;
  double m_levelEma = 0;
  double m_integral = 0;
  double m_ppm = 0;
  int m_appliedSteps = 0;

  ReceiveStats m_local;
  mutable std::mutex m_statsMutex;
  ReceiveStats m_published;
};

} // namespace deskflow::audio
