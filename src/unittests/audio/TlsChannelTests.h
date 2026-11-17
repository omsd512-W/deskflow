/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TestCertificates.h"

#include <QTest>

class TlsChannelTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void test_handshakeAndMessages();
  void test_closeIsSeenByPeer();
  void test_garbageIsRejected();

private:
  TestCertificates m_certificates;
};
