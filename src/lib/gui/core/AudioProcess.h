/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace deskflow::gui {

/**
 * Runs deskflow-audio, the audio sharing helper, as a child of the GUI.
 *
 * As a child it runs as the logged-in user in their session (where the audio
 * devices are), and on macOS the permission to record system audio is asked
 * for and kept on behalf of Deskflow. The helper reads its settings from
 * Deskflow's settings file and follows changes by itself; this class starts
 * it, restarts it if it crashes, relays its log and status, and passes
 * commands both ways (see AudioService::handleCommand).
 */
class AudioProcess : public QObject
{
  Q_OBJECT
public:
  explicit AudioProcess(QObject *parent = nullptr);
  ~AudioProcess() override;

  static QString helperPath();
  static bool isAvailable();
  //! Whether the helper has a job here: always on the server, which plays and
  //! manages its clients, and on a Mac client, which may be told to start.
  static bool isNeeded();

  //! Keep the helper running (and restart it when it exits) or stop it.
  void setWanted(bool wanted);
  bool isRunning() const;
  //! The helper's latest status report (see AudioService::status()).
  QJsonObject status() const
  {
    return m_status;
  }

  //! Output and input device names, from the helper. Blocks for up to a few seconds.
  static QPair<QStringList, QStringList> listDevices();

  //! The computer the cursor is on (server); kept for a restarted helper too.
  void setActiveComputer(const QString &name);
  //! Change one of a client's shared audio settings (server).
  void setClientSetting(const QString &client, const QString &key, bool value);

Q_SIGNALS:
  void statusChanged(const QJsonObject &status);
  void logLine(const QString &line);
  //! The server changed one of this computer's audio settings; save it.
  void settingRequested(const QString &key, bool value);

private:
  void start();
  void stop();
  void readOutput();
  void onFinished(int exitCode, QProcess::ExitStatus status);
  void setStatus(const QJsonObject &status);
  void sendCommand(const char *command, const QJsonObject &argument);

  bool m_wanted = false;
  QProcess *m_process = nullptr;
  QTimer m_restartTimer;
  int m_restartDelayMs = 1000;
  QByteArray m_pending;
  QJsonObject m_status;
  QString m_activeComputer;
};

} // namespace deskflow::gui
