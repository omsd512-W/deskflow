/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioControl.h"

#include "common/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QJsonArray>
#include <QMenu>
#include <QRegularExpression>

namespace deskflow::gui {

namespace {

// The client switches, as the helper names them (see AudioSettingsSync.h).
const auto kForward = QStringLiteral("forwardSystemAudio");
const auto kKeepLocal = QStringLiteral("keepLocalPlayback");
const auto kRemoteMicrophone = QStringLiteral("useRemoteMicrophone");

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

QString latencyText(const QJsonObject &stream)
{
  return stream.contains(QStringLiteral("latencyMs"))
             ? QStringLiteral(" (%1 ms)").arg(qRound(stream.value(QStringLiteral("latencyMs")).toDouble()))
             : QString();
}

} // namespace

AudioControl::AudioControl(QWidget *parent) : QObject(parent), m_menu(new QMenu(parent))
{
  using A = Settings::Audio;
  m_forward = makeToggle(m_menu, A::ForwardSystemAudio);
  m_keepLocal = makeToggle(m_menu, A::KeepLocalPlayback);
  m_remoteMicrophone = makeToggle(m_menu, A::UseRemoteMicrophone);

  m_mixModes = new QActionGroup(this);
  m_mixAll = m_menu->addAction(QString());
  m_followCursor = m_menu->addAction(QString());
  for (auto *action : {m_mixAll, m_followCursor}) {
    action->setCheckable(true);
    m_mixModes->addAction(action);
  }
  connect(m_mixAll, &QAction::triggered, this, [] { Settings::setValue(A::MixMode, QStringLiteral("mix")); });
  connect(m_followCursor, &QAction::triggered, this, [] {
    Settings::setValue(A::MixMode, QStringLiteral("followCursor"));
  });
  m_menu->addSeparator();
  // Client submenus go here, before this separator.
  m_clientsSeparator = m_menu->addSeparator();
  m_status = m_menu->addAction(QString());
  m_status->setEnabled(false);

  connect(Settings::instance(), &Settings::settingsChanged, this, &AudioControl::settingsChanged);
  connect(&m_process, &AudioProcess::logLine, this, &AudioControl::logLine);
  connect(&m_process, &AudioProcess::statusChanged, this, [this] {
    updateActions();
    Q_EMIT summaryChanged(summary());
  });
  connect(&m_process, &AudioProcess::settingRequested, this, [this](const QString &key, bool value) {
    if (Settings::value(key).toBool() != value) {
      Settings::setValue(key, value);
    }
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
  //: Server: every client's sound plays at once
  m_mixAll->setText(tr("Mix every client's sound"));
  //: Server: only the sound of the computer the cursor is on plays
  m_followCursor->setText(tr("Only play the computer the cursor is on"));
  for (auto &[name, menu] : m_clientMenus) {
    retranslateClientMenu(name, menu);
  }
  updateActions();
}

void AudioControl::setCoreRunning(bool running)
{
  if (running != m_coreRunning) {
    // A server that starts again has the cursor at home, wherever it was.
    m_process.setActiveComputer(QString());
  }
  m_coreRunning = running;
  update();
}

void AudioControl::handleCoreLogLine(const QString &line)
{
  static const QRegularExpression switched(QString::fromLatin1("switch from \"(.*)\" to \"(.*)\" at -?\\d+,-?\\d+"));
  if (!line.contains(QStringLiteral("switch from")) || isClient()) {
    return;
  }
  if (const auto match = switched.match(line); match.hasMatch()) {
    m_process.setActiveComputer(match.captured(2));
  }
}

void AudioControl::settingsChanged(const QString &key)
{
  if (key.startsWith(QStringLiteral("audio/")) || key == Settings::Core::CoreMode) {
    update();
  }
}

void AudioControl::update()
{
  m_process.setWanted(m_coreRunning && AudioProcess::isAvailable() && AudioProcess::isNeeded());
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
  m_keepLocal->setEnabled(m_forward->isChecked());

  const bool followCursor = Settings::value(A::MixMode).toString() == QStringLiteral("followCursor");
  for (auto *action : {m_mixAll, m_followCursor}) {
    const QSignalBlocker block(action);
    action->setVisible(!client);
  }
  (followCursor ? m_followCursor : m_mixAll)->setChecked(true);
  updateClientMenus(client ? QJsonObject() : m_process.status());

  m_menu->menuAction()->setVisible(AudioProcess::isAvailable() && (!client || canForward));
  const auto text = summary();
  m_status->setText(text.isEmpty() ? tr("Audio sharing is off") : text);
}

AudioControl::ClientMenu &AudioControl::clientMenu(const QString &name)
{
  if (const auto it = m_clientMenus.find(name); it != m_clientMenus.end()) {
    return it->second;
  }
  ClientMenu menu;
  menu.menu = new QMenu(m_menu);
  const auto toggle = [this, &menu, name](const QString &key) {
    auto *action = menu.menu->addAction(QString());
    action->setCheckable(true);
    connect(action, &QAction::toggled, this, [this, name, key](bool on) { m_process.setClientSetting(name, key, on); });
    return action;
  };
  menu.forward = toggle(kForward);
  menu.keepLocal = toggle(kKeepLocal);
  menu.remoteMicrophone = toggle(kRemoteMicrophone);
  menu.menu->addSeparator();
  menu.state = menu.menu->addAction(QString());
  menu.state->setEnabled(false);
  m_menu->insertMenu(m_clientsSeparator, menu.menu);
  retranslateClientMenu(name, menu);
  return m_clientMenus.emplace(name, menu).first->second;
}

void AudioControl::retranslateClientMenu(const QString &name, ClientMenu &menu)
{
  Q_UNUSED(name)
  //: Server, a client's switch A: its sound plays here
  menu.forward->setText(tr("Play its sound on this computer"));
  //: Server, a client's switch B: the client keeps playing its own sound too
  menu.keepLocal->setText(tr("Keep playing it there too"));
  //: Server, a client's switch C: this computer's microphone appears on the client
  menu.remoteMicrophone->setText(tr("Let it use this computer's microphone"));
}

void AudioControl::updateClientMenus(const QJsonObject &status)
{
  QStringList seen;
  for (const auto &value : status.value(QStringLiteral("clients")).toArray()) {
    const auto client = value.toObject();
    const auto name = client.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) {
      continue;
    }
    seen << name;
    auto &menu = clientMenu(name);
    const bool online = client.value(QStringLiteral("online")).toBool();
    const auto settings = client.value(QStringLiteral("settings")).toObject();
    const auto pending = client.value(QStringLiteral("pending")).toArray();
    for (const auto &[action, key] :
         {std::pair{menu.forward, kForward}, std::pair{menu.keepLocal, kKeepLocal},
          std::pair{menu.remoteMicrophone, kRemoteMicrophone}}) {
      const QSignalBlocker block(action);
      action->setChecked(settings.value(key).toBool());
    }
    menu.keepLocal->setEnabled(menu.forward->isChecked());

    //: A client in the server's Audio menu that is not connected now
    menu.menu->setTitle(online ? name : tr("%1 (offline)").arg(name));
    QString state;
    if (!pending.isEmpty()) {
      state = online ? tr("Waiting for it to confirm the change") : tr("Offline, takes effect when it next connects");
    } else if (!online) {
      state = tr("Offline");
    } else if (client.value(QStringLiteral("sending")).toBool()) {
      state = client.value(QStringLiteral("audible")).toBool()
                  ? tr("Playing here") + latencyText(client)
                  : tr("Sending sound, silent while the cursor is elsewhere");
    } else {
      state = tr("Connected");
    }
    if (client.value(QStringLiteral("microphone")).toBool()) {
      state += tr(", using the microphone");
    }
    menu.state->setText(state);
    menu.menu->menuAction()->setVisible(true);
  }
  for (auto &[name, menu] : m_clientMenus) {
    if (!seen.contains(name)) {
      menu.menu->menuAction()->setVisible(false);
    }
  }
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

  QStringList parts;
  if (status.value(QStringLiteral("role")).toString() == QStringLiteral("client")) {
    if (state == QStringLiteral("waiting") || (!connected && micState == QStringLiteral("waiting"))) {
      parts << tr("waiting for the server");
    } else if (state == QStringLiteral("forwarding")) {
      parts << tr("sound on %1").arg(peer) + latencyText(system);
    } else if (state == QStringLiteral("refused")) {
      parts << tr("%1 is not playing sound").arg(peer);
    } else if (state == QStringLiteral("undelivered")) {
      parts << tr("sound not reaching %1, playing here").arg(peer);
    } else if (state == QStringLiteral("error")) {
      parts << tr("cannot record this computer's sound");
    }
    if (micState == QStringLiteral("active")) {
      parts << tr("microphone of %1").arg(peer) + latencyText(microphone);
    } else if (micState == QStringLiteral("notInstalled")) {
      parts << tr("microphone driver not installed");
    }
  } else {
    QStringList playing;
    for (const auto &value : status.value(QStringLiteral("clients")).toArray()) {
      const auto client = value.toObject();
      if (client.value(QStringLiteral("sending")).toBool() && client.value(QStringLiteral("audible")).toBool()) {
        playing << client.value(QStringLiteral("name")).toString() + latencyText(client);
      }
    }
    if (state == QStringLiteral("error")) {
      parts << tr("cannot play sound");
    } else if (state == QStringLiteral("playing") && !playing.isEmpty()) {
      parts << tr("playing sound from %1").arg(playing.join(QStringLiteral(", ")));
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
