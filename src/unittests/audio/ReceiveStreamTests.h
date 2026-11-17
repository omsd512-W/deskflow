/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ReceiveStreamTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_cleanNetwork();
  void test_clockDrift_data();
  void test_clockDrift();
  void test_lossAndJitter_data();
  void test_lossAndJitter();
  void test_microphoneLoss_data();
  void test_microphoneLoss();
  void test_longRun();
  void test_pauseAndResume();
  void test_burstIsCut();
  void test_oddDevicePeriod();
};
