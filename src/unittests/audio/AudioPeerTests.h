/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TestCertificates.h"

#include <QTest>

class AudioPeerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void test_streamsBothWays();
  void test_untrustedPeerIsRejected();
  void test_stopSendingClosesStream();
  void test_clientReconnects();

private:
  TestCertificates m_certificates;
};
