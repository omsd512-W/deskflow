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
 * Deskflow's settings file and follows changes by itself; this class only
 * starts it, restarts it if it crashes, and relays its log and status.
 */
class AudioProcess : public QObject
{
  Q_OBJECT
public:
  explicit AudioProcess(QObject *parent = nullptr);
  ~AudioProcess() override;

  static QString helperPath();
  static bool isAvailable();
  //! Whether any audio sharing switch for this computer's role is on.
  static bool anyFeatureEnabled();

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

Q_SIGNALS:
  void statusChanged(const QJsonObject &status);
  void logLine(const QString &line);

private:
  void start();
  void stop();
  void readOutput();
  void onFinished(int exitCode, QProcess::ExitStatus status);
  void setStatus(const QJsonObject &status);

  bool m_wanted = false;
  QProcess *m_process = nullptr;
  QTimer m_restartTimer;
  int m_restartDelayMs = 1000;
  QByteArray m_pending;
  QJsonObject m_status;
};

} // namespace deskflow::gui
