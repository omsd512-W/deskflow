/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/AudioSettingsSync.h"

#include "audio/ControlProtocol.h"

namespace deskflow::audio::shared {

const QStringList &keys()
{
  static const QStringList list{kForwardSystemAudio, kKeepLocalPlayback, kUseRemoteMicrophone};
  return list;
}

QJsonObject message(const Values &values)
{
  QJsonObject settings;
  QJsonObject modified;
  for (const auto &[key, value] : values) {
    settings.insert(key, value.value);
    modified.insert(key, double(value.modifiedMs));
  }
  return {{protocol::kType, protocol::kSettings}, {"values", settings}, {"modified", modified}};
}

std::optional<Values> parse(const QJsonObject &message)
{
  if (message.value(protocol::kType).toString() != protocol::kSettings) {
    return std::nullopt;
  }
  const auto settings = message.value("values").toObject();
  const auto modified = message.value("modified").toObject();
  Values values;
  for (const auto &key : keys()) {
    if (settings.value(key).isBool()) {
      values[key] = Value{settings.value(key).toBool(), qint64(modified.value(key).toDouble())};
    }
  }
  return values;
}

Merge merge(const Values &local, const Values &remote, bool remoteWinsTies)
{
  Merge result;
  for (const auto &key : keys()) {
    const auto mine = local.find(key);
    const auto theirs = remote.find(key);
    if (mine == local.end() && theirs == remote.end()) {
      continue;
    }
    if (theirs == remote.end()) {
      result.merged[key] = mine->second;
      result.newerHere << key;
      continue;
    }
    if (mine == local.end()) {
      result.merged[key] = theirs->second;
      result.fromRemote << key;
      continue;
    }
    const auto &a = mine->second;
    const auto &b = theirs->second;
    if (a == b) {
      result.merged[key] = a;
    } else if (b.modifiedMs > a.modifiedMs || (b.modifiedMs == a.modifiedMs && remoteWinsTies)) {
      result.merged[key] = b;
      result.fromRemote << key;
    } else {
      result.merged[key] = a;
      result.newerHere << key;
    }
  }
  return result;
}

} // namespace deskflow::audio::shared
