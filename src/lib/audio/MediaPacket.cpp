/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/MediaPacket.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>

namespace deskflow::audio {

namespace {

void putU32(std::uint8_t *out, std::uint32_t v)
{
  out[0] = std::uint8_t(v >> 24);
  out[1] = std::uint8_t(v >> 16);
  out[2] = std::uint8_t(v >> 8);
  out[3] = std::uint8_t(v);
}

void putU64(std::uint8_t *out, std::uint64_t v)
{
  putU32(out, std::uint32_t(v >> 32));
  putU32(out + 4, std::uint32_t(v));
}

std::uint32_t getU32(const std::uint8_t *in)
{
  return (std::uint32_t(in[0]) << 24) | (std::uint32_t(in[1]) << 16) | (std::uint32_t(in[2]) << 8) |
         std::uint32_t(in[3]);
}

std::uint64_t getU64(const std::uint8_t *in)
{
  return (std::uint64_t(getU32(in)) << 32) | getU32(in + 4);
}

// 96-bit nonce: 4-byte per-stream salt followed by the 64-bit sequence.
// Every stream gets a fresh random key, so (key, nonce) never repeats.
void makeNonce(const StreamKey &key, std::uint64_t sequence, std::uint8_t *nonce)
{
  std::memcpy(nonce, key.salt.data(), key.salt.size());
  putU64(nonce + 4, sequence);
}

} // namespace

void MediaHeader::write(std::uint8_t *out) const
{
  out[0] = kVersion;
  out[1] = type;
  out[2] = streamId;
  out[3] = flags;
  putU64(out + 4, sequence);
  putU32(out + 12, timestamp);
}

std::optional<MediaHeader> MediaHeader::read(const std::uint8_t *data, std::size_t size)
{
  if (size < std::size_t(kSize) || data[0] != kVersion) {
    return std::nullopt;
  }
  MediaHeader header;
  header.type = data[1];
  header.streamId = data[2];
  header.flags = data[3];
  header.sequence = getU64(data + 4);
  header.timestamp = getU32(data + 12);
  return header;
}

StreamKey StreamKey::generate()
{
  StreamKey key;
  RAND_bytes(key.key.data(), int(key.key.size()));
  RAND_bytes(key.salt.data(), int(key.salt.size()));
  return key;
}

QByteArray StreamKey::toBase64() const
{
  QByteArray raw(reinterpret_cast<const char *>(key.data()), int(key.size()));
  raw.append(reinterpret_cast<const char *>(salt.data()), int(salt.size()));
  return raw.toBase64();
}

std::optional<StreamKey> StreamKey::fromBase64(const QByteArray &text)
{
  const auto raw = QByteArray::fromBase64(text);
  StreamKey result;
  if (raw.size() != int(result.key.size() + result.salt.size())) {
    return std::nullopt;
  }
  std::memcpy(result.key.data(), raw.constData(), result.key.size());
  std::memcpy(result.salt.data(), raw.constData() + result.key.size(), result.salt.size());
  return result;
}

PacketSealer::PacketSealer(const StreamKey &key) : m_key(key), m_ctx(EVP_CIPHER_CTX_new())
{
  EVP_EncryptInit_ex(m_ctx, EVP_aes_256_gcm(), nullptr, m_key.key.data(), nullptr);
}

PacketSealer::~PacketSealer()
{
  EVP_CIPHER_CTX_free(m_ctx);
}

int PacketSealer::seal(
    const MediaHeader &header, const std::uint8_t *payload, int size, std::uint8_t *out, int capacity
)
{
  if (size < 0 || capacity < MediaHeader::kSize + size + kTagSize) {
    return -1;
  }
  header.write(out);
  std::uint8_t nonce[12];
  makeNonce(m_key, header.sequence, nonce);
  int length = 0;
  if (EVP_EncryptInit_ex(m_ctx, nullptr, nullptr, nullptr, nonce) != 1 ||
      EVP_EncryptUpdate(m_ctx, nullptr, &length, out, MediaHeader::kSize) != 1 ||
      EVP_EncryptUpdate(m_ctx, out + MediaHeader::kSize, &length, payload, size) != 1 ||
      EVP_EncryptFinal_ex(m_ctx, out + MediaHeader::kSize + length, &length) != 1 ||
      EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, out + MediaHeader::kSize + size) != 1) {
    return -1;
  }
  return MediaHeader::kSize + size + kTagSize;
}

PacketOpener::PacketOpener(const StreamKey &key) : m_key(key), m_ctx(EVP_CIPHER_CTX_new())
{
  EVP_DecryptInit_ex(m_ctx, EVP_aes_256_gcm(), nullptr, m_key.key.data(), nullptr);
}

PacketOpener::~PacketOpener()
{
  EVP_CIPHER_CTX_free(m_ctx);
}

int PacketOpener::open(
    const std::uint8_t *datagram, int size, MediaHeader &header, std::uint8_t *payload, int capacity
)
{
  const auto parsed = MediaHeader::read(datagram, size);
  const int payloadSize = size - MediaHeader::kSize - kTagSize;
  if (!parsed || payloadSize < 0 || payloadSize > capacity) {
    return -1;
  }
  std::uint8_t nonce[12];
  makeNonce(m_key, parsed->sequence, nonce);
  // OpenSSL takes the expected tag through a non-const pointer.
  std::uint8_t tag[kTagSize];
  std::memcpy(tag, datagram + size - kTagSize, kTagSize);
  int length = 0;
  if (EVP_DecryptInit_ex(m_ctx, nullptr, nullptr, nullptr, nonce) != 1 ||
      EVP_DecryptUpdate(m_ctx, nullptr, &length, datagram, MediaHeader::kSize) != 1 ||
      EVP_DecryptUpdate(m_ctx, payload, &length, datagram + MediaHeader::kSize, payloadSize) != 1 ||
      EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tag) != 1 ||
      EVP_DecryptFinal_ex(m_ctx, payload + length, &length) != 1) {
    return -1;
  }
  header = *parsed;
  return payloadSize;
}

bool ReplayWindow::isFresh(std::uint64_t sequence) const
{
  if (m_empty || sequence > m_highest) {
    return true;
  }
  const auto age = m_highest - sequence;
  if (age >= 64) {
    return false;
  }
  return (m_seen & (std::uint64_t(1) << age)) == 0;
}

void ReplayWindow::accept(std::uint64_t sequence)
{
  if (m_empty) {
    m_empty = false;
    m_highest = sequence;
    m_seen = 1;
    return;
  }
  if (sequence > m_highest) {
    const auto shift = sequence - m_highest;
    m_seen = shift >= 64 ? 0 : (m_seen << shift);
    m_seen |= 1;
    m_highest = sequence;
    return;
  }
  const auto age = m_highest - sequence;
  if (age < 64) {
    m_seen |= std::uint64_t(1) << age;
  }
}

} // namespace deskflow::audio
