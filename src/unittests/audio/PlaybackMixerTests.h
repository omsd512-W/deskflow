/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class PlaybackMixerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_mixesEveryone();
  void test_clampsLoudMix();
  void test_followsCursor();
  void test_fadesWhenCursorMoves();
  void test_unknownComputerPlaysEveryone();
  void test_silentSourceIsStillPulled();
};
