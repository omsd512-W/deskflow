/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioControl.h"

#include "common/Settings.h"

#include <QAction>
#include <QMenu>

namespace deskflow::gui {

namespace {

bool isClient()
{
  return Settings::value(Settings::Core::CoreMode).toInt() == Settings::CoreMode::Client;
}

QAction *makeToggle(QMenu *menu, const QString &key)
{
  auto *action = menu->addAction(QString());
  action->setCheckable(true);
  QObject::connect(action, &QAction::toggled, menu, [key](bool on) {
    if (Settings::value(key).toBool() != on) {
      Settings::setValue(key, on);
    }
  });
  return action;
}

} // namespace

AudioControl::AudioControl(QWidget *parent) : QObject(parent), m_menu(new QMenu(parent))
{
  using A = Settings::Audio;
  m_forward = makeToggle(m_menu, A::ForwardSystemAudio);
  m_keepLocal = makeToggle(m_menu, A::KeepLocalPlayback);
  m_remoteMicrophone = makeToggle(m_menu, A::UseRemoteMicrophone);
  m_allowPlayback = makeToggle(m_menu, A::AllowRemotePlayback);
  m_allowMicrophone = makeToggle(m_menu, A::AllowMicrophoneSharing);
  m_menu->addSeparator();
  m_status = m_menu->addAction(QString());
  m_status->setEnabled(false);

  connect(Settings::instance(), &Settings::settingsChanged, this, &AudioControl::settingsChanged);
  connect(&m_process, &AudioProcess::logLine, this, &AudioControl::logLine);
  connect(&m_process, &AudioProcess::statusChanged, this, [this] {
    updateActions();
    Q_EMIT summaryChanged(summary());
  });

  retranslate();
  update();
}

void AudioControl::retranslate()
{
  m_menu->setTitle(tr("&Audio"));
  //: Switch A: the Mac's sound plays on the server's headset
  m_forward->setText(tr("Play this computer's sound on the server"));
  //: Switch B: keep the Mac's own speakers playing as well
  m_keepLocal->setText(tr("Keep playing it here too"));
  //: Switch C: the server's microphone appears on this Mac as an input device
  m_remoteMicrophone->setText(tr("Use the server's microphone"));
  m_allowPlayback->setText(tr("Play sound sent by clients"));
  m_allowMicrophone->setText(tr("Share this computer's microphone with clients"));
  updateActions();
}

void AudioControl::setCoreRunning(bool running)
{
  m_coreRunning = running;
  update();
}

void AudioControl::settingsChanged(const QString &key)
{
  if (key.startsWith(QStringLiteral("audio/")) || key == Settings::Core::CoreMode) {
    update();
  }
}

void AudioControl::update()
{
  m_process.setWanted(m_coreRunning && AudioProcess::isAvailable() && AudioProcess::anyFeatureEnabled());
  updateActions();
  Q_EMIT summaryChanged(summary());
}

void AudioControl::updateActions()
{
  using A = Settings::Audio;
  const bool client = isClient();
#if defined(Q_OS_MACOS)
  const bool canForward = true;
#else
  const bool canForward = false;
#endif
  const auto sync = [](QAction *action, const QString &key, bool visible) {
    const QSignalBlocker block(action);
    action->setVisible(visible);
    action->setChecked(Settings::value(key).toBool());
  };
  sync(m_forward, A::ForwardSystemAudio, client && canForward);
  sync(m_keepLocal, A::KeepLocalPlayback, client && canForward);
  sync(m_remoteMicrophone, A::UseRemoteMicrophone, client && canForward);
  sync(m_allowPlayback, A::AllowRemotePlayback, !client);
  sync(m_allowMicrophone, A::AllowMicrophoneSharing, !client);
  m_keepLocal->setEnabled(m_forward->isChecked());
  m_menu->menuAction()->setVisible(AudioProcess::isAvailable() && (!client || canForward));

  const auto text = summary();
  m_status->setText(text.isEmpty() ? tr("Audio sharing is off") : text);
}

QString AudioControl::summary() const
{
  if (!m_process.isRunning()) {
    return {};
  }
  return describe(m_process.status());
}

QString AudioControl::describe(const QJsonObject &status) const
{
  if (status.isEmpty()) {
    return tr("Audio: starting");
  }
  const bool connected = status.value(QStringLiteral("connected")).toBool();
  const auto peer = status.value(QStringLiteral("peer")).toString();
  const auto system = status.value(QStringLiteral("system")).toObject();
  const auto microphone = status.value(QStringLiteral("microphone")).toObject();
  const auto state = system.value(QStringLiteral("state")).toString();
  const auto micState = microphone.value(QStringLiteral("state")).toString();
  const auto latency = [](const QJsonObject &stream) {
    return stream.contains(QStringLiteral("latencyMs"))
               ? QStringLiteral(" (%1 ms)").arg(qRound(stream.value(QStringLiteral("latencyMs")).toDouble()))
               : QString();
  };

  QStringList parts;
  if (status.value(QStringLiteral("role")).toString() == QStringLiteral("client")) {
    if (state == QStringLiteral("waiting") || (!connected && micState == QStringLiteral("waiting"))) {
      parts << tr("waiting for the server");
    } else if (state == QStringLiteral("forwarding")) {
      parts << tr("sound on %1").arg(peer) + latency(system);
    } else if (state == QStringLiteral("refused")) {
      parts << tr("%1 is not playing sound").arg(peer);
    } else if (state == QStringLiteral("undelivered")) {
      parts << tr("sound not reaching %1, playing here").arg(peer);
    } else if (state == QStringLiteral("error")) {
      parts << tr("cannot record this computer's sound");
    }
    if (micState == QStringLiteral("active")) {
      parts << tr("microphone of %1").arg(peer) + latency(microphone);
    } else if (micState == QStringLiteral("notInstalled")) {
      parts << tr("microphone driver not installed");
    }
  } else {
    if (state == QStringLiteral("playing")) {
      parts << tr("playing sound from %1").arg(peer) + latency(system);
    } else if (state == QStringLiteral("error")) {
      parts << tr("cannot play sound");
    } else if (connected) {
      parts << tr("connected to %1").arg(peer);
    } else {
      parts << tr("waiting for a client");
    }
    if (micState == QStringLiteral("sharing")) {
      parts << tr("microphone shared");
    } else if (micState == QStringLiteral("error")) {
      parts << tr("cannot record the microphone");
    }
  }
  if (parts.isEmpty()) {
    return {};
  }
  //: Status bar text, followed by a list such as "sound on Windows-PC (35 ms)"
  return tr("Audio: %1").arg(parts.join(QStringLiteral(" · ")));
}

} // namespace deskflow::gui
