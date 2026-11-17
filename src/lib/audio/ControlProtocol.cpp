/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/ControlProtocol.h"

namespace deskflow::audio::protocol {

std::optional<StreamKind> kindFromString(const QString &text)
{
  if (text == QLatin1String(toString(StreamKind::System))) {
    return StreamKind::System;
  }
  if (text == QLatin1String(toString(StreamKind::Microphone))) {
    return StreamKind::Microphone;
  }
  return std::nullopt;
}

QJsonObject hello(const QString &name, quint16 udpPort)
{
  return {{kType, kHello}, {"version", kVersion}, {"name", name}, {"udpPort", int(udpPort)}};
}

std::optional<Hello> parseHello(const QJsonObject &message)
{
  if (message.value(kType).toString() != kHello) {
    return std::nullopt;
  }
  Hello result;
  result.version = message.value("version").toInt();
  result.name = message.value("name").toString();
  const int port = message.value("udpPort").toInt();
  if (port <= 0 || port > 65535) {
    return std::nullopt;
  }
  result.udpPort = quint16(port);
  return result;
}

QJsonObject streamOffer(std::uint8_t id, const StreamFormat &format, const StreamKey &key)
{
  return {
      {kType, kStreamOffer},
      {"id", int(id)},
      {"kind", QString::fromLatin1(toString(format.kind))},
      {"channels", format.channels},
      {"frameSamples", format.frameSamples},
      {"bitrate", format.bitrate},
      {"codec", "opus"},
      {"key", QString::fromLatin1(key.toBase64())},
  };
}

std::optional<StreamOffer> parseStreamOffer(const QJsonObject &message)
{
  if (message.value(kType).toString() != kStreamOffer || message.value("codec").toString() != "opus") {
    return std::nullopt;
  }
  const auto kind = kindFromString(message.value("kind").toString());
  const auto key = StreamKey::fromBase64(message.value("key").toString().toLatin1());
  const int id = message.value("id").toInt(-1);
  const int channels = message.value("channels").toInt();
  const int frameSamples = message.value("frameSamples").toInt();
  // Opus frame sizes at 48 kHz: 2.5, 5, 10, 20, 40 or 60 ms.
  const bool validFrame = frameSamples == 120 || frameSamples == 240 || frameSamples == 480 || frameSamples == 960 ||
                          frameSamples == 1920 || frameSamples == 2880;
  if (!kind || !key || id < 0 || id > 255 || channels < 1 || channels > 2 || !validFrame) {
    return std::nullopt;
  }
  StreamOffer offer;
  offer.id = std::uint8_t(id);
  offer.format.kind = *kind;
  offer.format.channels = channels;
  offer.format.frameSamples = frameSamples;
  offer.format.bitrate = message.value("bitrate").toInt();
  offer.key = *key;
  return offer;
}

QJsonObject streamClose(std::uint8_t id)
{
  return {{kType, kStreamClose}, {"id", int(id)}};
}

QJsonObject demand(StreamKind kind, bool active)
{
  return {{kType, kDemand}, {"kind", QString::fromLatin1(toString(kind))}, {"active", active}};
}

// Microsecond clocks fit a double exactly for centuries of uptime.
QJsonObject ping(std::int64_t timeUs)
{
  return {{kType, kPing}, {"t", double(timeUs)}};
}

QJsonObject pong(std::int64_t echoedUs, std::int64_t responderUs)
{
  return {{kType, kPong}, {"t", double(echoedUs)}, {"r", double(responderUs)}};
}

QJsonObject timing(std::uint8_t id, std::uint32_t timestamp, std::int64_t captureUs)
{
  return {{kType, kTiming}, {"id", int(id)}, {"ts", double(timestamp)}, {"us", double(captureUs)}};
}

QJsonObject stats(std::uint8_t id, const ReceiveStats &stats)
{
  return {
      {kType, kStats},
      {"id", int(id)},
      {"packets", double(stats.packets)},
      {"concealed", double(stats.concealed)},
      {"late", double(stats.late)},
      {"dropped", double(stats.dropped)},
      {"underruns", double(stats.underruns)},
      {"resamplerErrors", double(stats.resamplerErrors)},
      {"bufferMs", stats.bufferMs},
      {"targetMs", stats.targetMs},
      {"jitterMs", stats.jitterMs},
      {"ratioPpm", stats.ratioPpm},
      {"playing", stats.playing},
  };
}

} // namespace deskflow::audio::protocol
