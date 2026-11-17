/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "gui/core/AudioProcess.h"

#include <QObject>

class QAction;
class QMenu;
class QWidget;

namespace deskflow::gui {

/**
 * The audio sharing switches in the tray and menu bar, and the helper process
 * behind them. The helper runs while Deskflow runs and a switch for this
 * computer's role is on.
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
  //! One line for the status bar, empty when audio sharing is off.
  QString summary() const;
  void retranslate();

Q_SIGNALS:
  void logLine(const QString &line);
  void summaryChanged(const QString &summary);

private:
  void settingsChanged(const QString &key);
  void update();
  void updateActions();
  QString describe(const QJsonObject &status) const;

  AudioProcess m_process;
  bool m_coreRunning = false;
  QMenu *m_menu = nullptr;
  QAction *m_forward = nullptr;
  QAction *m_keepLocal = nullptr;
  QAction *m_remoteMicrophone = nullptr;
  QAction *m_allowPlayback = nullptr;
  QAction *m_allowMicrophone = nullptr;
  QAction *m_status = nullptr;
};

} // namespace deskflow::gui
