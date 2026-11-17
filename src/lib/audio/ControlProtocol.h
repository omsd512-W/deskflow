/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"
#include "audio/MediaPacket.h"
#include "audio/ReceiveStream.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace deskflow::audio::protocol {

//! Bumped on incompatible changes; peers with a different version disconnect.
//! 2: a server takes several clients, and they share their audio settings.
inline constexpr int kVersion = 2;

inline const QString kType = QStringLiteral("type");

// Message types.
inline const QString kHello = QStringLiteral("hello");             // version, name, udpPort
inline const QString kStreamOffer = QStringLiteral("streamOffer"); // id, kind, channels, frameSamples, bitrate, key
inline const QString kStreamClose = QStringLiteral("streamClose"); // id
inline const QString kDemand = QStringLiteral("demand");           // kind, active
inline const QString kPing = QStringLiteral("ping");               // t: sender clock, µs
inline const QString kPong = QStringLiteral("pong");               // t: echoed, r: responder clock, µs
inline const QString kStats = QStringLiteral("stats");             // id + ReceiveStats fields, latencyMs
//! Sample `ts` of stream `id` was captured at sender clock `us`.
inline const QString kTiming = QStringLiteral("timing");           // id, ts, us
//! A client's shared audio settings, see AudioSettingsSync.h.
inline const QString kSettings = QStringLiteral("settings"); // values, modified

struct Hello
{
  int version = 0;
  QString name;
  quint16 udpPort = 0;
};

struct StreamOffer
{
  std::uint8_t id = 0;
  StreamFormat format;
  StreamKey key;
};

QJsonObject hello(const QString &name, quint16 udpPort);
std::optional<Hello> parseHello(const QJsonObject &message);

QJsonObject streamOffer(std::uint8_t id, const StreamFormat &format, const StreamKey &key);
std::optional<StreamOffer> parseStreamOffer(const QJsonObject &message);

QJsonObject streamClose(std::uint8_t id);
QJsonObject demand(StreamKind kind, bool active);
QJsonObject ping(std::int64_t timeUs);
QJsonObject pong(std::int64_t echoedUs, std::int64_t responderUs);
QJsonObject timing(std::uint8_t id, std::uint32_t timestamp, std::int64_t captureUs);
QJsonObject stats(std::uint8_t id, const ReceiveStats &stats);

std::optional<StreamKind> kindFromString(const QString &text);

} // namespace deskflow::audio::protocol
