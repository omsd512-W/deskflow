/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioProcess.h"

#include "common/Constants.h"
#include "common/Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

namespace deskflow::gui {

namespace {

constexpr int kMaxRestartDelayMs = 30000;
constexpr int kStopTimeoutMs = 3000;

QString logPath()
{
  const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir().mkpath(dir);
  return QStringLiteral("%1/%2-audio.log").arg(dir, kAppId);
}

} // namespace

AudioProcess::AudioProcess(QObject *parent) : QObject(parent)
{
  m_restartTimer.setSingleShot(true);
  connect(&m_restartTimer, &QTimer::timeout, this, &AudioProcess::start);
}

AudioProcess::~AudioProcess()
{
  m_wanted = false;
  stop();
}

QString AudioProcess::helperPath()
{
#if defined(Q_OS_WIN)
  return QStringLiteral("%1/%2-audio.exe").arg(QCoreApplication::applicationDirPath(), kAppId);
#else
  return QStringLiteral("%1/%2-audio").arg(QCoreApplication::applicationDirPath(), kAppId);
#endif
}

bool AudioProcess::isAvailable()
{
  return QFile::exists(helperPath());
}

bool AudioProcess::anyFeatureEnabled()
{
  using A = Settings::Audio;
  if (Settings::value(Settings::Core::CoreMode).toInt() == Settings::CoreMode::Client) {
#if defined(Q_OS_MACOS)
    return Settings::value(A::ForwardSystemAudio).toBool() || Settings::value(A::UseRemoteMicrophone).toBool();
#else
    return false; // forwarding from this computer is only implemented on macOS
#endif
  }
  return Settings::value(A::AllowRemotePlayback).toBool() || Settings::value(A::AllowMicrophoneSharing).toBool();
}

void AudioProcess::setWanted(bool wanted)
{
  if (wanted == m_wanted) {
    return;
  }
  m_wanted = wanted;
  if (wanted) {
    m_restartDelayMs = 1000;
    start();
  } else {
    m_restartTimer.stop();
    stop();
  }
}

bool AudioProcess::isRunning() const
{
  return m_process && m_process->state() != QProcess::NotRunning;
}

void AudioProcess::start()
{
  if (!m_wanted || isRunning()) {
    return;
  }
  if (!isAvailable()) {
    Q_EMIT logLine(QStringLiteral("[audio] helper not found at %1").arg(helperPath()));
    return;
  }
  if (m_process) {
    m_process->deleteLater();
  }
  m_process = new QProcess(this);
  m_process->setProcessChannelMode(QProcess::MergedChannels);
  connect(m_process, &QProcess::readyReadStandardOutput, this, &AudioProcess::readOutput);
  connect(m_process, &QProcess::finished, this, &AudioProcess::onFinished);
  connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      Q_EMIT logLine(QStringLiteral("[audio] cannot start the helper: %1").arg(m_process->errorString()));
      onFinished(-1, QProcess::CrashExit);
    }
  });
  const QStringList args{
      QStringLiteral("--log-file"), logPath(), QStringLiteral("--parent-pid"),
      QString::number(QCoreApplication::applicationPid())
  };
  m_process->start(helperPath(), args);
}

void AudioProcess::stop()
{
  if (!isRunning()) {
    return;
  }
  auto *process = m_process;
  m_process = nullptr;
  process->disconnect(this);
#if defined(Q_OS_WIN)
  // Without a window it would not see the close request; it holds nothing
  // that outlives it, so just end it.
  process->kill();
  process->waitForFinished(1000);
#else
  // Lets the helper take its tap down, so the Mac plays locally right away.
  process->terminate();
  if (!process->waitForFinished(kStopTimeoutMs)) {
    process->kill();
    process->waitForFinished(1000);
  }
#endif
  process->deleteLater();
  setStatus({});
  Q_EMIT logLine(QStringLiteral("[audio] helper stopped"));
}

void AudioProcess::readOutput()
{
  if (!m_process) {
    return;
  }
  m_pending += m_process->readAllStandardOutput();
  qsizetype end;
  while ((end = m_pending.indexOf('\n')) >= 0) {
    auto line = QString::fromUtf8(m_pending.left(end)).trimmed();
    m_pending.remove(0, end + 1);
    if (line.startsWith(QStringLiteral("STATUS "))) {
      const auto doc = QJsonDocument::fromJson(line.mid(7).toUtf8());
      if (doc.isObject()) {
        setStatus(doc.object());
        // Healthy for a while: next crash restarts quickly again.
        m_restartDelayMs = 1000;
      }
    } else if (!line.isEmpty()) {
      // The helper prefixes its own time of day; the log view has its own.
      static const QRegularExpression time(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d\\.\\d\\d\\d "));
      line.remove(time);
      Q_EMIT logLine(QStringLiteral("[%1] audio: %2")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODate), line));
    }
  }
}

void AudioProcess::onFinished(int exitCode, QProcess::ExitStatus status)
{
  readOutput();
  setStatus({});
  if (!m_wanted) {
    return;
  }
  Q_EMIT logLine(
      QStringLiteral("[audio] helper exited (%1), restarting in %2 s")
          .arg(status == QProcess::CrashExit ? QStringLiteral("crashed") : QString::number(exitCode))
          .arg(m_restartDelayMs / 1000)
  );
  m_restartTimer.start(m_restartDelayMs);
  m_restartDelayMs = std::min(m_restartDelayMs * 2, kMaxRestartDelayMs);
}

void AudioProcess::setStatus(const QJsonObject &status)
{
  if (status == m_status) {
    return;
  }
  m_status = status;
  Q_EMIT statusChanged(m_status);
}

QPair<QStringList, QStringList> AudioProcess::listDevices()
{
  QProcess process;
  process.start(helperPath(), {QStringLiteral("--list-devices")});
  if (!process.waitForFinished(5000)) {
    process.kill();
    return {};
  }
  const auto doc = QJsonDocument::fromJson(process.readAllStandardOutput());
  const auto toList = [](const QJsonValue &value) {
    QStringList names;
    for (const auto &name : value.toArray()) {
      names.append(name.toString());
    }
    return names;
  };
  return {toList(doc.object().value(QStringLiteral("output"))), toList(doc.object().value(QStringLiteral("input")))};
}

} // namespace deskflow::gui
