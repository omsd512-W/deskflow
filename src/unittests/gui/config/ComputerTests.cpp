/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerTests.h"

#include "common/Settings.h"
#include "gui/config/Computer.h"

void ComputerTests::initTestCase()
{
  QDir dir;
  QVERIFY(dir.mkpath(m_settingsPath));

  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists())
    oldSettings.remove();

  Settings::setSettingsFile(m_settingsFile);
  Settings::setStateFile(m_stateFile);
}

void ComputerTests::basicFunctionality()
{
  Computer computer;
  QVERIFY(computer.isNull());

  computer.setName("stub");
  QVERIFY(!computer.isNull());

  computer.saveSettings(Settings::proxy());
  computer.loadSettings(Settings::proxy());
  QCOMPARE("stub", computer.name());
}

void ComputerTests::loadSettings_savedGeometry_restored()
{
  Computer saved("laptop");
  saved.setGeometry(QRect(-1920, 0, 1920, 1080));
  saved.saveSettings(Settings::proxy());

  Computer loaded;
  loaded.loadSettings(Settings::proxy());

  QCOMPARE(loaded.geometry(), QRect(-1920, 0, 1920, 1080));
}

void ComputerTests::setMonitors_desktopRects_storedRelativeToGeometry()
{
  Computer server("desk");
  server.setGeometry(QRect(100, 200, 1920, 1080));

  // an L whose top monitor starts 12 pixels left of the bottom right one, as Windows reports them
  server.setMonitors({QRect(0, 0, 2560, 1440), QRect(2560, 0, 2560, 1080), QRect(2548, -1080, 1920, 1080)});

  QCOMPARE(server.geometry(), QRect(100, 200, 5120, 2520));
  QCOMPARE(
      server.monitors(),
      QList<QRect>({QRect(0, 1080, 2560, 1440), QRect(2560, 1080, 2560, 1080), QRect(2548, 0, 1920, 1080)})
  );
  QCOMPARE(
      server.layoutRects(),
      QList<QRect>({QRect(100, 1280, 2560, 1440), QRect(2660, 1280, 2560, 1080), QRect(2648, 200, 1920, 1080)})
  );
}

void ComputerTests::loadSettings_savedMonitors_restored()
{
  Computer saved("desk");
  saved.setMonitors({QRect(0, 0, 2560, 1440), QRect(2560, 0, 2560, 1080)});
  saved.saveSettings(Settings::proxy());

  Computer loaded;
  loaded.loadSettings(Settings::proxy());

  QCOMPARE(loaded.monitors(), saved.monitors());
  QCOMPARE(loaded, saved);
}

QTEST_MAIN(ComputerTests)
