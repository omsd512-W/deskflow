/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioPeer.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QTimer>

#include <memory>
#include <mutex>
#include <optional>

namespace deskflow::audio {

class AudioDevice;
namespace macos {
class SystemAudioCapture;
class VirtualMicrophone;
} // namespace macos

/**
 * The audio helper proper. Follows Deskflow's settings file:
 *
 * - On the client (the Mac), sends what it plays to the server while
 *   "forward system audio" is on and the server wants it, optionally muting
 *   it locally; and presents the server's microphone as a local input device
 *   while "use remote microphone" is on and some app is recording from it.
 * - On the server (the computer with the headset), plays what the client
 *   sends, and records its microphone while the client asks for it.
 *
 * Whenever sharing stops for any reason (switched off, disconnected, the
 * other side unable to play) the Mac's own speakers come back by themselves.
 */
class AudioService : public QObject
{
  Q_OBJECT
public:
  explicit AudioService(QObject *parent = nullptr);
  ~AudioService() override;

  //! For tests: accept peers from this fingerprint database instead of Deskflow's.
  void setTrustedDbOverride(const QString &path)
  {
    m_trustedDbOverride = path;
  }
  void start();
  QJsonObject status() const;

Q_SIGNALS:
  void statusChanged();
  void logMessage(const QString &line);

private:
  //! A stream handed between the main thread and an audio device thread.
  template <typename T> class Slot
  {
  public:
    std::shared_ptr<T> get() const
    {
      std::lock_guard lock(m_mutex);
      return m_value;
    }
    void set(std::shared_ptr<T> value)
    {
      std::shared_ptr<T> old;
      std::lock_guard lock(m_mutex);
      old.swap(m_value);
      m_value = std::move(value);
    }
    void reset()
    {
      set(nullptr);
    }

  private:
    mutable std::mutex m_mutex;
    std::shared_ptr<T> m_value;
  };

  struct Config
  {
    PeerConfig::Role role = PeerConfig::Role::Server;
    bool enabled = false; //!< Deskflow is set up as a client or a server
    QString host;
    quint16 port = 24801;
    QString certificate;
    QString trustedDb;
    QString name;
    bool forwardSystemAudio = false;
    bool keepLocalPlayback = false;
    bool useRemoteMicrophone = false;
    bool allowRemotePlayback = true;
    bool allowMicrophoneSharing = true;
    QString outputDevice;
    QString inputDevice;

    bool sameConnection(const Config &other) const
    {
      return enabled == other.enabled && role == other.role && host == other.host && port == other.port &&
             certificate == other.certificate && trustedDb == other.trustedDb && name == other.name;
    }
  };

  //! Notices a device whose callbacks have stopped (sleep, unplugging, a stuck driver).
  struct Watchdog
  {
    std::uint64_t count = 0;
    qint64 sinceMs = -1;
    bool stalled(bool active, std::uint64_t now, qint64 nowMs, qint64 limitMs)
    {
      if (!active) {
        sinceMs = -1;
        return false;
      }
      if (sinceMs < 0 || now != count) {
        count = now;
        sinceMs = nowMs;
        return false;
      }
      if (nowMs - sinceMs > limitMs) {
        sinceMs = -1;
        return true;
      }
      return false;
    }
  };

  Config readConfig();
  void checkWatchdogs(qint64 now);
  void reload();
  void watchSettings();
  void createPeer();
  void stopAll();
  void update();
  void tick();
  void log(const QString &line);

  // Client side (sound comes from here).
  bool captureRunning() const;
  void updateSystemCapture();
  void checkDelivery(qint64 now);
  void updateMicrophoneDemand();
  void updateMicrophoneFeed();
  // Server side (the headset is here).
  void updatePlayback();
  void updateRecording();
  void sendServerDemands();

  QString m_settingsPath;
  QString m_trustedDbOverride;
  std::unique_ptr<QSettings> m_settings;
  QFileSystemWatcher m_watcher;
  QTimer m_reloadTimer;
  QTimer m_tickTimer;
  Config m_config;
  std::unique_ptr<AudioPeer> m_peer;

  QElapsedTimer m_clock;

  // Client.
  bool m_peerWantsSystem = false;
  bool m_wantCapture = false;
  bool m_deliveryFailing = false;
  QString m_captureError;
  qint64 m_lastCaptureAttemptMs = 0;
  qint64 m_sendingSinceMs = 0;
  qint64 m_lastRemoteProgressMs = 0;
  std::uint64_t m_lastRemotePackets = 0;
  std::uint64_t m_sentAtRemoteProgress = 0;
  bool m_microphoneInUse = false;
  qint64 m_lastDeviceCheckMs = 0;
  bool m_sentMicrophoneDemand = false;

  // Server.
  std::optional<bool> m_sentSystemDemand;
  bool m_playbackFailing = false;
  QString m_playbackError;
  qint64 m_lastPlaybackAttemptMs = 0;
  qint64 m_playbackIdleSinceMs = 0;
  bool m_peerWantsMicrophone = false;
  bool m_wantRecording = false;
  QString m_recordError;
  qint64 m_lastRecordAttemptMs = 0;

  Watchdog m_captureWatch;
  Watchdog m_feedWatch;
  Watchdog m_playbackWatch;
  Watchdog m_recordWatch;

  // Declared before the devices, whose callbacks use them.
  Slot<SendStream> m_systemSend;
  Slot<SendStream> m_microphoneSend;
  Slot<ReceiveStream> m_systemReceive;
  Slot<ReceiveStream> m_microphoneReceive;

#if defined(Q_OS_MACOS)
  std::unique_ptr<macos::SystemAudioCapture> m_capture;
  std::unique_ptr<macos::VirtualMicrophone> m_microphone;
#endif
  std::unique_ptr<AudioDevice> m_playback;
  std::unique_ptr<AudioDevice> m_recorder;
};

} // namespace deskflow::audio
