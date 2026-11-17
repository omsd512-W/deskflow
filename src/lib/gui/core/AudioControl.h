/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "gui/core/AudioProcess.h"

#include <QObject>

#include <map>

class QAction;
class QActionGroup;
class QMenu;
class QWidget;

namespace deskflow::gui {

/**
 * The audio sharing switches in the tray and menu bar, and the helper process
 * behind them, which runs while Deskflow runs.
 *
 * On a client: this computer's three switches. On the server: how to play
 * several clients' sound, and every client's switches, which the server can
 * change too.
 */
class AudioControl : public QObject
{
  Q_OBJECT
public:
  explicit AudioControl(QWidget *parent);

  QMenu *menu() const
  {
    return m_menu;
  }
  void setCoreRunning(bool running);
  //! A line from the core's log, to follow the cursor between computers.
  void handleCoreLogLine(const QString &line);
  //! One line for the status bar, empty when audio sharing is off.
  QString summary() const;
  void retranslate();

Q_SIGNALS:
  void logLine(const QString &line);
  void summaryChanged(const QString &summary);

private:
  //! One client's switches, in a submenu of the server's Audio menu.
  struct ClientMenu
  {
    QMenu *menu = nullptr;
    QAction *forward = nullptr;
    QAction *keepLocal = nullptr;
    QAction *remoteMicrophone = nullptr;
    QAction *state = nullptr;
  };

  void settingsChanged(const QString &key);
  void update();
  void updateActions();
  void updateClientMenus(const QJsonObject &status);
  ClientMenu &clientMenu(const QString &name);
  void retranslateClientMenu(const QString &name, ClientMenu &menu);
  QString describe(const QJsonObject &status) const;

  AudioProcess m_process;
  bool m_coreRunning = false;
  QMenu *m_menu = nullptr;
  QAction *m_forward = nullptr;
  QAction *m_keepLocal = nullptr;
  QAction *m_remoteMicrophone = nullptr;
  QActionGroup *m_mixModes = nullptr;
  QAction *m_mixAll = nullptr;
  QAction *m_followCursor = nullptr;
  QAction *m_clientsSeparator = nullptr;
  std::map<QString, ClientMenu> m_clientMenus;
  QAction *m_status = nullptr;
};

} // namespace deskflow::gui
