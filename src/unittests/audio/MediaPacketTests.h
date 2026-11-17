/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class MediaPacketTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_headerRoundTrip();
  void test_headerRejectsMalformed();
  void test_sealAndOpen();
  void test_tamperingIsDetected();
  void test_wrongKeyIsRejected();
  void test_replayWindow();
  void test_keyBase64();
};
