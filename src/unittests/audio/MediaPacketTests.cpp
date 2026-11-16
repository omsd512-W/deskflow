/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MediaPacketTests.h"

#include "audio/AudioTypes.h"
#include "audio/MediaPacket.h"

#include <array>
#include <vector>

using namespace deskflow::audio;

namespace {

MediaHeader sampleHeader()
{
  MediaHeader header;
  header.streamId = 2;
  header.flags = MediaHeader::Discontinuity;
  header.sequence = 0x0102030405060708ull;
  header.timestamp = 0xA0B0C0D0u;
  return header;
}

std::vector<std::uint8_t> samplePayload()
{
  std::vector<std::uint8_t> payload(200);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::uint8_t(i * 7 + 3);
  }
  return payload;
}

} // namespace

void MediaPacketTests::test_headerRoundTrip()
{
  std::array<std::uint8_t, MediaHeader::kSize> buffer{};
  sampleHeader().write(buffer.data());

  // Big endian on the wire.
  QCOMPARE(buffer[0], MediaHeader::kVersion);
  QCOMPARE(buffer[4], std::uint8_t(0x01));
  QCOMPARE(buffer[11], std::uint8_t(0x08));
  QCOMPARE(buffer[12], std::uint8_t(0xA0));

  const auto read = MediaHeader::read(buffer.data(), buffer.size());
  QVERIFY(read.has_value());
  QCOMPARE(read->type, std::uint8_t(MediaHeader::Media));
  QCOMPARE(read->streamId, std::uint8_t(2));
  QCOMPARE(read->flags, std::uint8_t(MediaHeader::Discontinuity));
  QCOMPARE(read->sequence, 0x0102030405060708ull);
  QCOMPARE(read->timestamp, 0xA0B0C0D0u);
}

void MediaPacketTests::test_headerRejectsMalformed()
{
  std::array<std::uint8_t, MediaHeader::kSize> buffer{};
  sampleHeader().write(buffer.data());
  QVERIFY(!MediaHeader::read(buffer.data(), buffer.size() - 1));
  buffer[0] = MediaHeader::kVersion + 1;
  QVERIFY(!MediaHeader::read(buffer.data(), buffer.size()));
}

void MediaPacketTests::test_sealAndOpen()
{
  const auto key = StreamKey::generate();
  PacketSealer sealer(key);
  PacketOpener opener(key);
  const auto payload = samplePayload();

  std::array<std::uint8_t, kMaxDatagramBytes> datagram{};
  const int size = sealer.seal(sampleHeader(), payload.data(), int(payload.size()), datagram.data(), int(datagram.size()));
  QCOMPARE(size, MediaHeader::kSize + int(payload.size()) + kTagSize);

  // Encrypted: the payload does not appear in the clear.
  QVERIFY(!std::equal(payload.begin(), payload.end(), datagram.begin() + MediaHeader::kSize));

  MediaHeader header;
  std::array<std::uint8_t, kMaxPayloadBytes> opened{};
  const int openedSize = opener.open(datagram.data(), size, header, opened.data(), int(opened.size()));
  QCOMPARE(openedSize, int(payload.size()));
  QVERIFY(std::equal(payload.begin(), payload.end(), opened.begin()));
  QCOMPARE(header.sequence, sampleHeader().sequence);

  // Too small an output buffer is refused rather than overrun.
  QCOMPARE(opener.open(datagram.data(), size, header, opened.data(), int(payload.size()) - 1), -1);
  // A payload that does not fit a datagram is refused.
  std::vector<std::uint8_t> huge(kMaxDatagramBytes);
  QCOMPARE(sealer.seal(sampleHeader(), huge.data(), int(huge.size()), datagram.data(), int(datagram.size())), -1);
}

void MediaPacketTests::test_tamperingIsDetected()
{
  const auto key = StreamKey::generate();
  PacketSealer sealer(key);
  PacketOpener opener(key);
  const auto payload = samplePayload();
  std::array<std::uint8_t, kMaxDatagramBytes> datagram{};
  const int size = sealer.seal(sampleHeader(), payload.data(), int(payload.size()), datagram.data(), int(datagram.size()));

  MediaHeader header;
  std::array<std::uint8_t, kMaxPayloadBytes> opened{};
  // Header (authenticated, not encrypted), ciphertext and tag.
  for (const int position : {3, 9, MediaHeader::kSize + 10, size - 1}) {
    auto copy = datagram;
    copy[std::size_t(position)] ^= 0x20;
    QCOMPARE(opener.open(copy.data(), size, header, opened.data(), int(opened.size())), -1);
  }
  QCOMPARE(opener.open(datagram.data(), size - 1, header, opened.data(), int(opened.size())), -1);
  QCOMPARE(opener.open(datagram.data(), MediaHeader::kSize + kTagSize - 1, header, opened.data(), int(opened.size())), -1);
  // The original still opens: a failed attempt leaves no state behind.
  QCOMPARE(opener.open(datagram.data(), size, header, opened.data(), int(opened.size())), int(payload.size()));
}

void MediaPacketTests::test_wrongKeyIsRejected()
{
  PacketSealer sealer(StreamKey::generate());
  PacketOpener opener(StreamKey::generate());
  const auto payload = samplePayload();
  std::array<std::uint8_t, kMaxDatagramBytes> datagram{};
  const int size = sealer.seal(sampleHeader(), payload.data(), int(payload.size()), datagram.data(), int(datagram.size()));

  MediaHeader header;
  std::array<std::uint8_t, kMaxPayloadBytes> opened{};
  QCOMPARE(opener.open(datagram.data(), size, header, opened.data(), int(opened.size())), -1);
}

void MediaPacketTests::test_replayWindow()
{
  ReplayWindow window;
  QVERIFY(window.isFresh(100));
  window.accept(100);
  QVERIFY(!window.isFresh(100));

  // Reordered packets inside the window are fine, once.
  QVERIFY(window.isFresh(98));
  window.accept(98);
  QVERIFY(!window.isFresh(98));
  QVERIFY(window.isFresh(99));

  // Moving ahead keeps what was seen.
  window.accept(130);
  QVERIFY(!window.isFresh(100));
  QVERIFY(!window.isFresh(98));
  QVERIFY(window.isFresh(99));
  QVERIFY(window.isFresh(131));

  // Older than 64 packets behind the newest is refused.
  QVERIFY(window.isFresh(67));
  QVERIFY(!window.isFresh(66));

  // A large jump forgets everything older.
  window.accept(1000);
  QVERIFY(!window.isFresh(130));
  QVERIFY(window.isFresh(999));
  QVERIFY(!window.isFresh(1000));
}

void MediaPacketTests::test_keyBase64()
{
  const auto key = StreamKey::generate();
  const auto text = key.toBase64();
  const auto back = StreamKey::fromBase64(text);
  QVERIFY(back.has_value());
  QVERIFY(back->key == key.key);
  QVERIFY(back->salt == key.salt);

  // Two keys are never the same.
  QVERIFY(StreamKey::generate().key != key.key);

  QVERIFY(!StreamKey::fromBase64(QByteArray("not base64 at all!")));
  QVERIFY(!StreamKey::fromBase64(text.left(text.size() - 8)));
  QVERIFY(!StreamKey::fromBase64(QByteArray()));
}

QTEST_MAIN(MediaPacketTests)
