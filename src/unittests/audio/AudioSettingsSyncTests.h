/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class AudioSettingsSyncTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_messageRoundTrip();
  void test_parseIgnoresOtherMessages();
  void test_laterChangeWins();
  void test_tiesGoToTheClient();
  void test_missingKeysAreFilledIn();
  void test_serverChangeReachesClientAndBack();
};
