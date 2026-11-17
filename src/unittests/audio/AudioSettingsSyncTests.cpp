/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioSettingsSyncTests.h"

#include "audio/AudioSettingsSync.h"
#include "audio/ControlProtocol.h"

using namespace deskflow::audio;
using namespace deskflow::audio::shared;

void AudioSettingsSyncTests::test_messageRoundTrip()
{
  const Values values{
      {kForwardSystemAudio, {true, 1763400000123}},
      {kKeepLocalPlayback, {false, 0}},
      {kUseRemoteMicrophone, {true, 1763400000999}},
  };
  const auto parsed = parse(message(values));
  QVERIFY(parsed.has_value());
  QVERIFY(*parsed == values);
}

void AudioSettingsSyncTests::test_parseIgnoresOtherMessages()
{
  QVERIFY(!parse({{protocol::kType, QStringLiteral("ping")}}).has_value());
  // Unknown keys and non-boolean values are dropped, the rest kept.
  const QJsonObject message{
      {protocol::kType, protocol::kSettings},
      {"values", QJsonObject{{"somethingNew", true}, {kForwardSystemAudio, "yes"}, {kKeepLocalPlayback, true}}},
      {"modified", QJsonObject{{kKeepLocalPlayback, 5.0}}},
  };
  const auto parsed = parse(message);
  QVERIFY(parsed.has_value());
  QCOMPARE(parsed->size(), std::size_t(1));
  QVERIFY(parsed->at(kKeepLocalPlayback) == (Value{true, 5}));
}

void AudioSettingsSyncTests::test_laterChangeWins()
{
  const Values local{{kForwardSystemAudio, {true, 100}}, {kKeepLocalPlayback, {false, 300}}};
  const Values remote{{kForwardSystemAudio, {false, 200}}, {kKeepLocalPlayback, {true, 250}}};
  const auto result = merge(local, remote, false);
  QVERIFY(result.merged.at(kForwardSystemAudio) == (Value{false, 200}));
  QVERIFY(result.merged.at(kKeepLocalPlayback) == (Value{false, 300}));
  QCOMPARE(result.fromRemote, QStringList{kForwardSystemAudio});
  QCOMPARE(result.newerHere, QStringList{kKeepLocalPlayback});

  // Nothing to do when both sides agree.
  const auto same = merge(result.merged, result.merged, true);
  QVERIFY(same.fromRemote.isEmpty() && same.newerHere.isEmpty());
}

void AudioSettingsSyncTests::test_tiesGoToTheClient()
{
  const Values client{{kForwardSystemAudio, {true, 100}}};
  const Values server{{kForwardSystemAudio, {false, 100}}};
  // On the server, the client's report wins...
  const auto onServer = merge(server, client, true);
  QVERIFY(onServer.merged.at(kForwardSystemAudio).value);
  QVERIFY(onServer.newerHere.isEmpty());
  // ...and on the client its own value stays, and the server hears about it.
  const auto onClient = merge(client, server, false);
  QVERIFY(onClient.merged.at(kForwardSystemAudio).value);
  QCOMPARE(onClient.newerHere, QStringList{kForwardSystemAudio});
}

void AudioSettingsSyncTests::test_missingKeysAreFilledIn()
{
  const Values local{{kForwardSystemAudio, {true, 100}}};
  const Values remote{{kUseRemoteMicrophone, {true, 50}}};
  const auto result = merge(local, remote, false);
  QCOMPARE(result.merged.size(), std::size_t(2));
  QCOMPARE(result.fromRemote, QStringList{kUseRemoteMicrophone});
  QCOMPARE(result.newerHere, QStringList{kForwardSystemAudio});
}

void AudioSettingsSyncTests::test_serverChangeReachesClientAndBack()
{
  // The Mac reported its settings; then the server's user switches one off.
  Values client{{kForwardSystemAudio, {true, 100}}, {kUseRemoteMicrophone, {false, 100}}};
  Values server = client;
  server[kForwardSystemAudio] = {false, 500};

  // The server sends its view; the Mac takes the newer value...
  const auto onClient = merge(client, server, false);
  QCOMPARE(onClient.fromRemote, QStringList{kForwardSystemAudio});
  client = onClient.merged;
  QVERIFY(!client.at(kForwardSystemAudio).value);

  // ...and answers with its own view, which settles the server.
  const auto onServer = merge(server, client, true);
  QVERIFY(onServer.fromRemote.isEmpty() && onServer.newerHere.isEmpty());
  QCOMPARE(client.at(kForwardSystemAudio).modifiedMs, 500);
}

QTEST_MAIN(AudioSettingsSyncTests)
