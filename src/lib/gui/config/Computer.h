/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ComputerConfig.h"

#include "common/KeyboardModifier.h"
#include "common/QSettingsProxy.h"

#include <QIcon>
#include <QList>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>

class QSettings;
class QTextStream;
class ComputerSettingsDialog;

class Computer : public ComputerConfig
{
  friend class ComputerSettingsDialog;

  friend QDataStream &operator<<(QDataStream &outStream, const Computer &computer)
  {
    return outStream << computer.name() << computer.switchCornerSize() << computer.aliases() << computer.modifiers()
                     << computer.switchCorners() << computer.fixes() << computer.isServer() << computer.geometry()
                     << computer.monitors() << computer.mouseSpeed();
  }

  friend QDataStream &operator>>(QDataStream &inStream, Computer &computer)
  {
    return inStream >> computer.m_Name >> computer.m_SwitchCornerSize >> computer.m_Aliases >> computer.m_Modifiers >>
           computer.m_SwitchCorners >> computer.m_Fixes >> computer.m_isServer >> computer.m_geometry >>
           computer.m_monitors >> computer.m_mouseSpeed;
  }

public:
  explicit Computer(const QString &name = QString());

  [[nodiscard]] const QPixmap &pixmap() const
  {
    return m_Pixmap;
  }
  [[nodiscard]] const QString &name() const
  {
    return m_Name;
  }
  [[nodiscard]] const QStringList &aliases() const
  {
    return m_Aliases;
  }

  [[nodiscard]] bool isNull() const
  {
    return m_Name.isEmpty();
  }
  [[nodiscard]] int modifier(int m) const
  {
    return m_Modifiers[m] == static_cast<int>(KeyboardModifier::DefaultMod) ? m : m_Modifiers[m];
  }
  [[nodiscard]] const QList<int> &modifiers() const
  {
    return m_Modifiers;
  }
  [[nodiscard]] bool switchCorner(int c) const
  {
    return m_SwitchCorners[c];
  }
  [[nodiscard]] const QList<bool> &switchCorners() const
  {
    return m_SwitchCorners;
  }
  [[nodiscard]] int switchCornerSize() const
  {
    return m_SwitchCornerSize;
  }
  /**
   * @brief How much mouse motion is scaled on the computer, in percent.
   */
  [[nodiscard]] int mouseSpeed() const
  {
    return m_mouseSpeed;
  }
  [[nodiscard]] bool fix(const Fix f) const
  {
    return m_Fixes[static_cast<int8_t>(f)];
  }
  [[nodiscard]] const QList<bool> &fixes() const
  {
    return m_Fixes;
  }
  /**
   * @brief Position and size of the computer in the layout, in pixels.
   * Invalid when the computer has not been placed yet.
   */
  [[nodiscard]] const QRect &geometry() const
  {
    return m_geometry;
  }
  /**
   * @brief Each monitor's rect relative to the top left of geometry(), in pixels.
   * Empty when the computer is laid out as a single rect, which is the case for clients.
   */
  [[nodiscard]] const QList<QRect> &monitors() const
  {
    return m_monitors;
  }
  /**
   * @brief The rects the computer takes up in the layout: its monitors where it sits, or its geometry.
   */
  [[nodiscard]] QList<QRect> layoutRects() const;

  void loadSettings(QSettingsProxy &settings);
  void saveSettings(QSettingsProxy &settings) const;
  [[nodiscard]] QString screensSection() const;

  [[nodiscard]] bool swapped() const
  {
    return m_Swapped;
  }

  void setName(const QString &name)
  {
    m_Name = name;
  }
  void setGeometry(const QRect &geometry)
  {
    m_geometry = geometry;
  }
  /**
   * @brief Lays the computer out by @p monitors, given in desktop coordinates. They're stored relative to
   * their bounding rect, which becomes the size of geometry(), keeping its position.
   */
  void setMonitors(const QList<QRect> &monitors);
  [[nodiscard]] bool isServer() const
  {
    return m_isServer;
  }
  void markAsServer()
  {
    m_isServer = true;
  }

  bool operator==(const Computer &computer) const;

protected:
  QStringList &aliases()
  {
    return m_Aliases;
  }
  void setModifier(const KeyboardModifier m, const int n)
  {
    m_Modifiers[static_cast<int8_t>(m)] = n;
  }
  QList<int> &modifiers()
  {
    return m_Modifiers;
  }
  void addAlias(const QString &alias)
  {
    m_Aliases.append(alias);
  }
  void setSwitchCorner(const SwitchCorner c, const bool on)
  {
    m_SwitchCorners[static_cast<int8_t>(c)] = on;
  }
  QList<bool> &switchCorners()
  {
    return m_SwitchCorners;
  }
  void setSwitchCornerSize(const int val)
  {
    m_SwitchCornerSize = val;
  }
  void setMouseSpeed(const int percent)
  {
    m_mouseSpeed = percent;
  }
  void setFix(const Fix f, const bool on)
  {
    m_Fixes[static_cast<int8_t>(f)] = on;
  }
  QList<bool> &fixes()
  {
    return m_Fixes;
  }
  void setSwapped(const bool on)
  {
    m_Swapped = on;
  }

private:
  QPixmap m_Pixmap = QIcon::fromTheme("video-display").pixmap(QSize(96, 96));
  QString m_Name = {};
  QStringList m_Aliases = {};
  QList<int> m_Modifiers = {0, 1, 2, 3, 4, 5, 6};
  QList<bool> m_SwitchCorners = {false, false, false, false};
  int m_SwitchCornerSize = 0;
  int m_mouseSpeed = 100;
  QList<bool> m_Fixes{false, false, false, false};
  bool m_Swapped = false;
  bool m_isServer = false;
  QRect m_geometry = {};
  QList<QRect> m_monitors = {};
};
