/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioService.h"

#include "audio/AudioDevice.h"
#include "common/Settings.h"

#if defined(Q_OS_MACOS)
#include "audio/macos/SystemAudioCapture.h"
#include "audio/macos/VirtualMicrophone.h"
#endif

#include <QFileInfo>
#include <QHostInfo>

namespace deskflow::audio {

namespace {

QJsonObject receiveSummary(const ReceiveStream &stream)
{
  const auto stats = stream.stats();
  return {
      {"packets", double(stats.packets)},     {"concealed", double(stats.concealed)},
      {"late", double(stats.late)},           {"underruns", double(stats.underruns)},
      {"bufferMs", stats.bufferMs},           {"targetMs", stats.targetMs},
      {"jitterMs", stats.jitterMs},
  };
}

//! How long the server keeps its output device open with nothing to play.
constexpr qint64 kIdlePlaybackMs = 10000;
//! How long the client sends before expecting the server to report packets.
constexpr qint64 kDeliveryGraceMs = 3000;
//! Retry interval for a device that would not open.
constexpr qint64 kRetryMs = 3000;

QString stripPort(QString host)
{
  host = host.trimmed();
  if (host.startsWith('[')) {
    const auto end = host.indexOf(']');
    return end > 0 ? host.mid(1, end - 1) : host;
  }
  if (host.count(':') == 1) {
    return host.section(':', 0, 0);
  }
  return host;
}

} // namespace

AudioService::AudioService(QObject *parent) : QObject(parent)
{
  m_clock.start();
  m_reloadTimer.setSingleShot(true);
  m_reloadTimer.setInterval(300);
  connect(&m_reloadTimer, &QTimer::timeout, this, &AudioService::reload);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
  m_tickTimer.setInterval(1000);
  connect(&m_tickTimer, &QTimer::timeout, this, &AudioService::tick);
}

AudioService::~AudioService()
{
  // Stop capture first so the Mac's speakers come back before anything else goes.
#if defined(Q_OS_MACOS)
  m_capture.reset();
  m_microphone.reset();
#endif
  m_playback.reset();
  m_recorder.reset();
  m_peer.reset();
}

void AudioService::start()
{
  m_settingsPath = Settings::settingsFile();
  m_settings = std::make_unique<QSettings>(m_settingsPath, QSettings::IniFormat);
  log(QStringLiteral("following settings in %1").arg(m_settingsPath));

#if defined(Q_OS_MACOS)
  macos::SystemAudioCapture::removeStaleTaps([this](const QString &line) { log(line); });
  m_capture = std::make_unique<macos::SystemAudioCapture>(
      [this](const float *pcm, int frames, Clock::time_point captured) {
        if (auto stream = m_systemSend.get()) {
          stream->push(pcm, frames, captured);
        }
      },
      [this](const QString &line) {
        QMetaObject::invokeMethod(this, [this, line] { log(line); }, Qt::QueuedConnection);
      }
  );
  m_microphone = std::make_unique<macos::VirtualMicrophone>(
      [this](float *out, int frames) {
        if (auto stream = m_microphoneReceive.get()) {
          stream->pull(out, frames);
        } else {
          std::fill(out, out + frames, 0.0f);
        }
      },
      [this](bool inUse) {
        QMetaObject::invokeMethod(
            this,
            [this, inUse] {
              m_microphoneInUse = inUse;
              log(inUse ? QStringLiteral("an app started recording from the Deskflow microphone")
                        : QStringLiteral("no app is recording from the Deskflow microphone"));
              updateMicrophoneDemand();
              Q_EMIT statusChanged();
            },
            Qt::QueuedConnection
        );
      },
      [this](const QString &line) {
        QMetaObject::invokeMethod(this, [this, line] { log(line); }, Qt::QueuedConnection);
      }
  );
  if (macos::VirtualMicrophone::isInstalled()) {
    m_microphone->watch();
  } else {
    log(QStringLiteral("the Deskflow microphone driver is not installed"));
  }
#endif

  m_playback = std::make_unique<AudioDevice>(AudioDevice::Direction::Playback, 2, [this](const QString &line) {
    QMetaObject::invokeMethod(this, [this, line] { log(line); }, Qt::QueuedConnection);
  });
  m_playback->setSource([this](float *out, int frames) {
    if (auto stream = m_systemReceive.get()) {
      stream->pull(out, frames);
    } else {
      std::fill(out, out + std::size_t(frames) * 2, 0.0f);
    }
  });
  m_recorder = std::make_unique<AudioDevice>(AudioDevice::Direction::Capture, 1, [this](const QString &line) {
    QMetaObject::invokeMethod(this, [this, line] { log(line); }, Qt::QueuedConnection);
  });
  m_recorder->setSink([this](const float *pcm, int frames, Clock::time_point captured) {
    if (auto stream = m_microphoneSend.get()) {
      stream->push(pcm, frames, captured);
    }
  });

  m_config = readConfig();
  watchSettings();
  createPeer();
  m_tickTimer.start();
}

AudioService::Config AudioService::readConfig()
{
  m_settings->sync();
  const auto value = [this](const QString &key) { return m_settings->value(key, Settings::defaultValue(key)); };

  Config config;
  const auto mode = value(Settings::Core::CoreMode).toInt();
  config.role = mode == Settings::CoreMode::Client ? PeerConfig::Role::Client : PeerConfig::Role::Server;
  config.host = stripPort(value(Settings::Client::RemoteHost).toString());
  config.port = quint16(value(Settings::Audio::Port).toUInt());
  if (config.port == 0) {
    config.port = quint16(value(Settings::Core::Port).toUInt() + 1);
  }
  config.certificate = value(Settings::Security::Certificate).toString();
  config.trustedDb = config.role == PeerConfig::Role::Client ? Settings::tlsTrustedServersDb()
                                                             : Settings::tlsTrustedClientsDb();
  if (!m_trustedDbOverride.isEmpty()) {
    config.trustedDb = m_trustedDbOverride;
  }
  config.name = value(Settings::Core::ComputerName).toString();
  if (config.name.isEmpty()) {
    config.name = QHostInfo::localHostName();
  }
  config.enabled = (mode == Settings::CoreMode::Client && !config.host.isEmpty()) || mode == Settings::CoreMode::Server;
  config.forwardSystemAudio = value(Settings::Audio::ForwardSystemAudio).toBool();
  config.keepLocalPlayback = value(Settings::Audio::KeepLocalPlayback).toBool();
  config.useRemoteMicrophone = value(Settings::Audio::UseRemoteMicrophone).toBool();
  config.allowRemotePlayback = value(Settings::Audio::AllowRemotePlayback).toBool();
  config.allowMicrophoneSharing = value(Settings::Audio::AllowMicrophoneSharing).toBool();
  config.outputDevice = value(Settings::Audio::OutputDevice).toString();
  config.inputDevice = value(Settings::Audio::InputDevice).toString();
  return config;
}

void AudioService::watchSettings()
{
  // Settings are saved by writing a new file and renaming it, which drops a
  // file watch; watching the directory too catches that.
  if (!m_watcher.files().contains(m_settingsPath) && QFileInfo::exists(m_settingsPath)) {
    m_watcher.addPath(m_settingsPath);
  }
  const auto directory = QFileInfo(m_settingsPath).absolutePath();
  if (!m_watcher.directories().contains(directory)) {
    m_watcher.addPath(directory);
  }
}

void AudioService::reload()
{
  watchSettings();
  const auto config = readConfig();
  const bool reconnect = !config.sameConnection(m_config);
  const auto old = m_config;
  m_config = config;
  if (reconnect) {
    log(QStringLiteral("connection settings changed"));
    stopAll();
    createPeer();
    return;
  }
  if (old.forwardSystemAudio != config.forwardSystemAudio || old.keepLocalPlayback != config.keepLocalPlayback ||
      old.useRemoteMicrophone != config.useRemoteMicrophone ||
      old.allowRemotePlayback != config.allowRemotePlayback ||
      old.allowMicrophoneSharing != config.allowMicrophoneSharing || old.outputDevice != config.outputDevice ||
      old.inputDevice != config.inputDevice) {
    log(QStringLiteral("audio settings changed"));
    if (old.outputDevice != config.outputDevice && m_playback->isRunning()) {
      m_playback->stop();
    }
    if (old.inputDevice != config.inputDevice && m_recorder->isRunning()) {
      m_recorder->stop();
    }
    update();
  }
}

void AudioService::stopAll()
{
#if defined(Q_OS_MACOS)
  m_capture->stop();
  m_microphone->stopFeeding();
#endif
  m_systemSend.reset();
  m_microphoneSend.reset();
  m_playback->stop();
  m_recorder->stop();
  m_systemReceive.reset();
  m_microphoneReceive.reset();
  m_peer.reset();
  m_peerWantsSystem = false;
  m_peerWantsMicrophone = false;
  m_sentMicrophoneDemand = false;
  m_deliveryFailing = false;
  Q_EMIT statusChanged();
}

void AudioService::createPeer()
{
  if (!m_config.enabled) {
    log(QStringLiteral("Deskflow is not set up as a client or a server, audio sharing is idle"));
    return;
  }
  if (!QFileInfo::exists(m_config.certificate)) {
    log(QStringLiteral("no TLS certificate at %1, audio sharing needs Deskflow's TLS to be set up")
            .arg(m_config.certificate));
    return;
  }

  PeerConfig peer;
  peer.role = m_config.role;
  peer.host = m_config.host;
  peer.port = m_config.port;
  peer.certificatePath = m_config.certificate;
  peer.trustedDbPath = m_config.trustedDb;
  peer.name = m_config.name;
  m_peer = std::make_unique<AudioPeer>(peer);
  connect(m_peer.get(), &AudioPeer::logMessage, this, &AudioService::log);
  connect(m_peer.get(), &AudioPeer::connected, this, [this] {
    m_sentMicrophoneDemand = false;
    m_deliveryFailing = false;
    update();
  });
  connect(m_peer.get(), &AudioPeer::disconnected, this, [this] {
    m_peerWantsSystem = false;
    m_peerWantsMicrophone = false;
    m_sentMicrophoneDemand = false;
    update();
  });
  connect(m_peer.get(), &AudioPeer::demandChanged, this, [this](StreamKind kind, bool active) {
    if (kind == StreamKind::System) {
      m_peerWantsSystem = active;
    } else {
      m_peerWantsMicrophone = active;
    }
    update();
  });
  connect(m_peer.get(), &AudioPeer::receiveStreamChanged, this, [this](StreamKind) { update(); });

  QString error;
  if (!m_peer->start(&error)) {
    log(QStringLiteral("cannot start audio sharing: %1").arg(error));
    m_peer.reset();
    return;
  }
  log(m_config.role == PeerConfig::Role::Client
          ? QStringLiteral("audio client for %1:%2").arg(m_config.host).arg(m_config.port)
          : QStringLiteral("audio server on port %1").arg(m_config.port));
  update();
}

void AudioService::update()
{
  if (m_config.role == PeerConfig::Role::Client) {
    updateSystemCapture();
    updateMicrophoneDemand();
    updateMicrophoneFeed();
  } else {
    sendServerDemands();
    updatePlayback();
    updateRecording();
  }
  Q_EMIT statusChanged();
}

void AudioService::tick()
{
  if (!m_peer) {
    return;
  }
  const auto now = m_clock.elapsed();
  checkWatchdogs(now);
  if (m_config.role == PeerConfig::Role::Client) {
#if defined(Q_OS_MACOS)
    if (now - m_lastDeviceCheckMs >= 5000) {
      m_lastDeviceCheckMs = now;
      m_microphone->refresh();
    }
#endif
    checkDelivery(now);
    if (m_wantCapture && !captureRunning() && now - m_lastCaptureAttemptMs >= kRetryMs) {
      updateSystemCapture();
    }
  } else {
    if (m_playbackFailing && now - m_lastPlaybackAttemptMs >= kRetryMs) {
      // Probe the device; if it opens, ask for the stream again.
      updatePlayback();
    }
    if (m_playback->isRunning() && !m_systemReceive.get()) {
      if (m_playbackIdleSinceMs == 0) {
        m_playbackIdleSinceMs = now;
      } else if (now - m_playbackIdleSinceMs > kIdlePlaybackMs) {
        log(QStringLiteral("nothing to play, closing the output device"));
        m_playback->stop();
        m_playbackIdleSinceMs = 0;
      }
    } else {
      m_playbackIdleSinceMs = 0;
    }
    if (m_playback->isRunning()) {
      m_peer->setOutputLatency(StreamKind::System, m_playback->latencyMs());
    } else if (m_systemReceive.get() && now - m_lastPlaybackAttemptMs >= kRetryMs) {
      updatePlayback(); // the device stopped by itself (unplugged)
    }
    if (m_wantRecording && !m_recorder->isRunning() && now - m_lastRecordAttemptMs >= kRetryMs) {
      updateRecording();
    }
  }
  Q_EMIT statusChanged();
}

void AudioService::checkWatchdogs(qint64 now)
{
  constexpr qint64 kStallMs = 3000;
#if defined(Q_OS_MACOS)
  // The tap only runs while something plays, so silence is not a stall.
  if (m_captureWatch.stalled(m_capture->isRunning() && m_capture->outputActive(), m_capture->callbacks(), now, kStallMs)) {
    log(QStringLiteral("system audio capture stopped delivering, starting it again"));
    m_capture->stop();
    updateSystemCapture();
  }
  if (m_feedWatch.stalled(m_microphone->isFeeding(), m_microphone->callbacks(), now, kStallMs)) {
    log(QStringLiteral("the Deskflow microphone stopped running, starting it again"));
    m_microphone->stopFeeding();
    updateMicrophoneFeed();
  }
#endif
  if (m_playbackWatch.stalled(m_playback->isRunning(), m_playback->callbacks(), now, kStallMs)) {
    log(QStringLiteral("the output device stopped playing, opening it again"));
    m_playback->stop();
    updatePlayback();
  }
  if (m_recordWatch.stalled(m_recorder->isRunning(), m_recorder->callbacks(), now, kStallMs)) {
    log(QStringLiteral("the input device stopped recording, opening it again"));
    m_recorder->stop();
    updateRecording();
  }
}

// --- client --------------------------------------------------------------

bool AudioService::captureRunning() const
{
#if defined(Q_OS_MACOS)
  return m_capture && m_capture->isRunning();
#else
  return false;
#endif
}

void AudioService::updateSystemCapture()
{
  m_wantCapture = m_peer && m_peer->isConnected() && m_config.forwardSystemAudio && m_peerWantsSystem;
#if defined(Q_OS_MACOS)
  const bool mute = !m_config.keepLocalPlayback && !m_deliveryFailing;
  if (!m_wantCapture) {
    // Capture goes first, so the Mac plays locally again before the stream closes.
    if (m_capture->isRunning()) {
      m_capture->stop();
      log(QStringLiteral("stopped forwarding system audio"));
    }
    m_systemSend.reset();
    if (m_peer) {
      m_peer->setSending(StreamKind::System, false);
    }
    m_sendingSinceMs = 0;
    return;
  }
  m_peer->setSending(StreamKind::System, true);
  m_systemSend.set(m_peer->sendStream(StreamKind::System));
  if (m_capture->isRunning()) {
    m_capture->setMuteLocal(mute);
    return;
  }
  m_lastCaptureAttemptMs = m_clock.elapsed();
  QString error;
  if (!m_capture->start(mute, &error)) {
    log(QStringLiteral("cannot capture system audio: %1").arg(error));
    m_captureError = error;
    return;
  }
  m_captureError.clear();
  m_sendingSinceMs = m_clock.elapsed();
  m_lastRemotePackets = 0;
  m_lastRemoteProgressMs = m_sendingSinceMs;
  if (const auto stream = m_systemSend.get()) {
    m_sentAtRemoteProgress = stream->stats().packets;
  }
  log(QStringLiteral("forwarding system audio to %1%2")
          .arg(m_peer->peerName(), mute ? QStringLiteral(", muted here") : QStringLiteral(", also playing here")));
#else
  if (m_wantCapture) {
    log(QStringLiteral("forwarding system audio is only implemented on macOS"));
  }
#endif
}

void AudioService::checkDelivery(qint64 now)
{
  // If the other side reports nothing arriving while we keep sending (a
  // firewall dropping UDP, say), stop muting this Mac so the sound does not
  // just vanish. Silence is not sent at all, so only packets we actually sent
  // count.
  const auto stream = m_systemSend.get();
  if (!captureRunning() || m_sendingSinceMs == 0 || !stream) {
    return;
  }
  const auto sent = stream->stats().packets;
  const auto stats = m_peer->remoteStats(StreamKind::System);
  const auto packets = std::uint64_t(stats.value(QStringLiteral("packets")).toDouble());
  if (packets > m_lastRemotePackets || packets < m_lastRemotePackets) {
    m_lastRemotePackets = packets;
    m_lastRemoteProgressMs = now;
    m_sentAtRemoteProgress = sent;
  }
  constexpr std::uint64_t kUnansweredPackets = 150; // 1.5 s of audio
  const bool failing = sent - m_sentAtRemoteProgress >= kUnansweredPackets && now - m_lastRemoteProgressMs > kDeliveryGraceMs;
  if (failing == m_deliveryFailing) {
    return;
  }
  m_deliveryFailing = failing;
  log(failing ? QStringLiteral("%1 is not receiving the audio (blocked by a firewall?), playing it here as well")
                    .arg(m_peer->peerName())
              : QStringLiteral("%1 is receiving the audio again").arg(m_peer->peerName()));
#if defined(Q_OS_MACOS)
  m_capture->setMuteLocal(!m_config.keepLocalPlayback && !m_deliveryFailing);
#endif
}

void AudioService::updateMicrophoneDemand()
{
  if (!m_peer || !m_peer->isConnected()) {
    return;
  }
  const bool want = m_config.useRemoteMicrophone && m_microphoneInUse;
  if (want != m_sentMicrophoneDemand) {
    m_peer->sendDemand(StreamKind::Microphone, want);
    m_sentMicrophoneDemand = want;
    log(want ? QStringLiteral("asking %1 for its microphone").arg(m_peer->peerName())
             : QStringLiteral("no longer need the microphone of %1").arg(m_peer->peerName()));
  }
}

void AudioService::updateMicrophoneFeed()
{
#if defined(Q_OS_MACOS)
  auto stream = m_peer ? m_peer->receiveStream(StreamKind::Microphone) : nullptr;
  m_microphoneReceive.set(stream);
  if (!stream) {
    m_microphone->stopFeeding();
    return;
  }
  if (!m_microphone->isFeeding()) {
    if (!m_microphone->watch()) {
      log(QStringLiteral("the Deskflow microphone driver is not installed"));
      return;
    }
    QString error;
    if (!m_microphone->startFeeding(&error)) {
      log(error);
      return;
    }
  }
  m_peer->setOutputLatency(StreamKind::Microphone, m_microphone->latencyMs());
#endif
}

// --- server --------------------------------------------------------------

void AudioService::sendServerDemands()
{
  if (!m_peer || !m_peer->isConnected()) {
    m_sentSystemDemand.reset();
    return;
  }
  const bool want = m_config.allowRemotePlayback && !m_playbackFailing;
  if (m_sentSystemDemand != want) {
    m_peer->sendDemand(StreamKind::System, want);
    m_sentSystemDemand = want;
  }
}

void AudioService::updatePlayback()
{
  auto stream = m_peer ? m_peer->receiveStream(StreamKind::System) : nullptr;
  m_systemReceive.set(stream);
  const bool probe = m_playbackFailing && m_config.allowRemotePlayback && m_peer && m_peer->isConnected();
  if (!stream && !probe) {
    return; // tick() closes the device once it has been idle for a while
  }
  if (m_playback->isRunning()) {
    return;
  }
  m_lastPlaybackAttemptMs = m_clock.elapsed();
  QString error;
  if (!m_playback->start(m_config.outputDevice, &error)) {
    if (!m_playbackFailing) {
      log(QStringLiteral("cannot play the audio: %1").arg(error));
      m_playbackFailing = true;
      m_playbackError = error;
      sendServerDemands(); // so the other computer plays it itself
    }
    return;
  }
  m_peer->setOutputLatency(StreamKind::System, m_playback->latencyMs());
  if (m_playbackFailing) {
    m_playbackFailing = false;
    m_playbackError.clear();
    sendServerDemands();
  }
}

void AudioService::updateRecording()
{
  m_wantRecording = m_peer && m_peer->isConnected() && m_config.allowMicrophoneSharing && m_peerWantsMicrophone;
  if (!m_wantRecording) {
    if (m_recorder->isRunning()) {
      m_recorder->stop();
      log(QStringLiteral("stopped sharing the microphone"));
    }
    m_microphoneSend.reset();
    if (m_peer) {
      m_peer->setSending(StreamKind::Microphone, false);
    }
    return;
  }
  m_peer->setSending(StreamKind::Microphone, true);
  m_microphoneSend.set(m_peer->sendStream(StreamKind::Microphone));
  if (m_recorder->isRunning()) {
    return;
  }
  m_lastRecordAttemptMs = m_clock.elapsed();
  QString error;
  if (!m_recorder->start(m_config.inputDevice, &error)) {
    if (m_recordError != error) {
      log(QStringLiteral("cannot record the microphone: %1").arg(error));
    }
    m_recordError = error;
    return;
  }
  m_recordError.clear();
  log(QStringLiteral("sharing the microphone with %1").arg(m_peer->peerName()));
}

// --- status --------------------------------------------------------------

QJsonObject AudioService::status() const
{
  QJsonObject status;
  status.insert(
      QStringLiteral("role"),
      m_config.role == PeerConfig::Role::Client ? QStringLiteral("client") : QStringLiteral("server")
  );
  status.insert(QStringLiteral("enabled"), m_config.enabled && m_peer != nullptr);
  const bool connected = m_peer && m_peer->isConnected();
  status.insert(QStringLiteral("connected"), connected);
  if (connected) {
    status.insert(QStringLiteral("peer"), m_peer->peerName());
    status.insert(QStringLiteral("roundTripMs"), m_peer->roundTripMs());
  }

  QJsonObject system;
  QJsonObject microphone;
  if (m_config.role == PeerConfig::Role::Client) {
    QString state = QStringLiteral("off");
    if (m_config.forwardSystemAudio) {
      if (!connected) {
        state = QStringLiteral("waiting");
      } else if (!m_peerWantsSystem) {
        state = QStringLiteral("refused"); // the other side cannot or will not play it
      } else if (!captureRunning()) {
        state = QStringLiteral("error");
      } else if (m_deliveryFailing) {
        state = QStringLiteral("undelivered");
      } else {
        state = QStringLiteral("forwarding");
      }
    }
    system.insert(QStringLiteral("state"), state);
    system.insert(QStringLiteral("mutedHere"), state == QStringLiteral("forwarding") && !m_config.keepLocalPlayback);
    if (!m_captureError.isEmpty() && state == QStringLiteral("error")) {
      system.insert(QStringLiteral("error"), m_captureError);
    }
    if (connected) {
      const auto remote = m_peer->remoteStats(StreamKind::System);
      if (remote.contains(QStringLiteral("latencyMs"))) {
        system.insert(QStringLiteral("latencyMs"), remote.value(QStringLiteral("latencyMs")));
      }
    }

    QString micState = QStringLiteral("off");
#if defined(Q_OS_MACOS)
    if (m_config.useRemoteMicrophone) {
      if (!macos::VirtualMicrophone::isInstalled()) {
        micState = QStringLiteral("notInstalled");
      } else if (!connected) {
        micState = QStringLiteral("waiting");
      } else if (!m_microphoneInUse) {
        micState = QStringLiteral("idle");
      } else if (m_microphone->isFeeding()) {
        micState = QStringLiteral("active");
      } else {
        micState = QStringLiteral("starting");
      }
    }
#endif
    microphone.insert(QStringLiteral("state"), micState);
    microphone.insert(QStringLiteral("inUse"), m_microphoneInUse);
    if (const auto stream = m_microphoneReceive.get()) {
      microphone.insert(QStringLiteral("receive"), receiveSummary(*stream));
    }
    if (connected) {
      if (const auto latency = m_peer->latencyMs(StreamKind::Microphone)) {
        microphone.insert(QStringLiteral("latencyMs"), *latency);
      }
    }
  } else {
    QString state = QStringLiteral("off");
    if (m_config.allowRemotePlayback) {
      if (m_playbackFailing) {
        state = QStringLiteral("error");
      } else if (m_systemReceive.get() && m_playback->isRunning()) {
        state = QStringLiteral("playing");
      } else {
        state = QStringLiteral("idle");
      }
    }
    system.insert(QStringLiteral("state"), state);
    if (m_playback->isRunning()) {
      system.insert(QStringLiteral("device"), m_playback->deviceName());
    }
    if (const auto stream = m_systemReceive.get()) {
      system.insert(QStringLiteral("receive"), receiveSummary(*stream));
    }
    if (m_playbackFailing) {
      system.insert(QStringLiteral("error"), m_playbackError);
    }
    if (connected) {
      if (const auto latency = m_peer->latencyMs(StreamKind::System)) {
        system.insert(QStringLiteral("latencyMs"), *latency);
      }
    }

    QString micState = QStringLiteral("off");
    if (m_config.allowMicrophoneSharing) {
      if (!m_wantRecording) {
        micState = QStringLiteral("idle");
      } else if (m_recorder->isRunning()) {
        micState = QStringLiteral("sharing");
      } else {
        micState = QStringLiteral("error");
      }
    }
    microphone.insert(QStringLiteral("state"), micState);
    if (m_recorder->isRunning()) {
      microphone.insert(QStringLiteral("device"), m_recorder->deviceName());
    }
    if (!m_recordError.isEmpty()) {
      microphone.insert(QStringLiteral("error"), m_recordError);
    }
  }
  status.insert(QStringLiteral("system"), system);
  status.insert(QStringLiteral("microphone"), microphone);
  return status;
}

void AudioService::log(const QString &line)
{
  Q_EMIT logMessage(line);
}

} // namespace deskflow::audio
