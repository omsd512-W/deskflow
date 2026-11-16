/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

struct evp_cipher_ctx_st;

namespace deskflow::audio {

/**
 * Header of one UDP media datagram. It is sent in clear, authenticated as
 * GCM additional data, and followed by the encrypted Opus payload and tag.
 *
 *   0  version (1)       4  sequence, 64-bit big endian
 *   1  type              12 timestamp, 32-bit big endian (sample clock)
 *   2  stream id
 *   3  flags
 */
struct MediaHeader
{
  static constexpr std::uint8_t kVersion = 1;
  static constexpr int kSize = 16;

  enum Type : std::uint8_t
  {
    Media = 1,
  };

  enum Flag : std::uint8_t
  {
    //! First packet after the sender paused or reset its encoder.
    Discontinuity = 0x01,
  };

  std::uint8_t type = Media;
  std::uint8_t streamId = 0;
  std::uint8_t flags = 0;
  std::uint64_t sequence = 0;
  std::uint32_t timestamp = 0;

  void write(std::uint8_t *out) const;
  static std::optional<MediaHeader> read(const std::uint8_t *data, std::size_t size);
};

//! AES-256-GCM key and nonce salt for one direction of one stream.
struct StreamKey
{
  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 4> salt{};

  static StreamKey generate();
  QByteArray toBase64() const;
  static std::optional<StreamKey> fromBase64(const QByteArray &text);
};

inline constexpr int kTagSize = 16;

//! Encrypts datagrams. Use from one thread at a time.
class PacketSealer
{
public:
  explicit PacketSealer(const StreamKey &key);
  ~PacketSealer();
  PacketSealer(const PacketSealer &) = delete;
  PacketSealer &operator=(const PacketSealer &) = delete;

  //! @return datagram size written to out, or -1
  int seal(const MediaHeader &header, const std::uint8_t *payload, int size, std::uint8_t *out, int capacity);

private:
  StreamKey m_key;
  evp_cipher_ctx_st *m_ctx = nullptr;
};

//! Decrypts and authenticates datagrams. Use from one thread at a time.
class PacketOpener
{
public:
  explicit PacketOpener(const StreamKey &key);
  ~PacketOpener();
  PacketOpener(const PacketOpener &) = delete;
  PacketOpener &operator=(const PacketOpener &) = delete;

  //! @return payload size, or -1 if the datagram is malformed or forged
  int open(const std::uint8_t *datagram, int size, MediaHeader &header, std::uint8_t *payload, int capacity);

private:
  StreamKey m_key;
  evp_cipher_ctx_st *m_ctx = nullptr;
};

//! Rejects replayed or very old sequence numbers (64-packet window).
class ReplayWindow
{
public:
  //! True if the sequence has not been accepted yet and is not too old.
  bool isFresh(std::uint64_t sequence) const;
  //! Record a sequence after its datagram was authenticated.
  void accept(std::uint64_t sequence);

private:
  bool m_empty = true;
  std::uint64_t m_highest = 0;
  std::uint64_t m_seen = 0; // bit n set: m_highest - n was accepted
};

} // namespace deskflow::audio
