/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class OpusCodecTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_roundTrip_data();
  void test_roundTrip();
  void test_concealment_data();
  void test_concealment();
};
