/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QJsonObject>
#include <QStringList>

#include <map>
#include <optional>

namespace deskflow::audio {

/**
 * The audio settings a client keeps for itself and the server can change too.
 *
 * Both sides may change a setting while the other is away, so every value
 * carries the time it was last changed (milliseconds since the epoch, UTC) and
 * the later change wins.
 */
namespace shared {

inline const QString kForwardSystemAudio = QStringLiteral("forwardSystemAudio");
inline const QString kKeepLocalPlayback = QStringLiteral("keepLocalPlayback");
inline const QString kUseRemoteMicrophone = QStringLiteral("useRemoteMicrophone");

//! Every shared setting, by its name in the protocol and under `audio/` in the settings.
const QStringList &keys();

struct Value
{
  bool value = false;
  qint64 modifiedMs = 0; //!< 0: never changed

  bool operator==(const Value &other) const = default;
};

using Values = std::map<QString, Value>;

QJsonObject message(const Values &values);
//! nullopt unless it is a settings message; unknown keys are dropped.
std::optional<Values> parse(const QJsonObject &message);

struct Merge
{
  Values merged;
  QStringList fromRemote; //!< keys whose value or time came from the other side
  QStringList newerHere;  //!< keys the other side should hear about
};

/**
 * Combine our values with the other side's, key by key, keeping the later
 * change. A key only one side has is taken from it.
 * \param remoteWinsTies whose value counts when both changed at the same time;
 * the server lets the client's own settings win.
 */
Merge merge(const Values &local, const Values &remote, bool remoteWinsTies);

} // namespace shared

} // namespace deskflow::audio
