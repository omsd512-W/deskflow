/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioPeer.h"
#include "audio/AudioSettingsSync.h"
#include "audio/PlaybackMixer.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QTimer>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace deskflow::audio {

class AudioDevice;
namespace macos {
class SystemAudioCapture;
class VirtualMicrophone;
} // namespace macos

/**
 * The audio helper proper. Follows Deskflow's settings file:
 *
 * - On a client (a Mac), sends what it plays to the server while
 *   "forward system audio" is on and the server wants it, optionally muting
 *   it locally; and presents the server's microphone as a local input device
 *   while "use remote microphone" is on and some app is recording from it.
 * - On the server (the computer with the headset), plays what its clients
 *   send, all mixed together or only the one the cursor is on, and records
 *   its microphone while any client asks for it.
 *
 * A client's three switches are its own settings, but the server shows and
 * changes them too; see AudioSettingsSync.h. A change from the server is
 * handed to the GUI (settingRequested), which saves it like any other.
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
  //! Where to keep the settings' change times and the clients' settings.
  void setStateDirectory(const QString &path)
  {
    m_stateDirectory = path;
  }
  void start();
  QJsonObject status() const;

  /**
   * A command from the GUI, one line each:
   * - `ACTIVE {"name": "..."}`: the cursor moved to this computer.
   * - `SET_CLIENT {"name": "...", "key": "...", "value": true}`: change a
   *   client's shared setting (server only).
   */
  void handleCommand(const QString &line);

Q_SIGNALS:
  void statusChanged();
  void logMessage(const QString &line);
  //! The server changed one of this client's settings; the GUI should save it.
  void settingRequested(const QString &key, bool value);

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
    PlaybackMixer::Mode mixMode = PlaybackMixer::Mode::Mix;
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

  //! What the server knows about one client's shared settings.
  struct ClientRecord
  {
    shared::Values values;
    //! The change time of each value the client last told us about.
    std::map<QString, qint64> reported;
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
  static bool configValue(const Config &config, const QString &key);
  static void setConfigValue(Config &config, const QString &key, bool value);
  //! Note which shared settings changed here since we last looked.
  void trackLocalSettings(bool starting);
  void sendLocalSettings();
  void onServerSettings(const shared::Values &values);

  // Server side (the headset is here).
  void updatePlayback();
  void updateRecording();
  void sendServerDemands(int link = -1);
  void loadClients();
  void saveClient(const QString &name);
  void onClientSettings(int link, const shared::Values &values);
  void setClientSetting(const QString &name, const QString &key, bool value);
  void setActiveComputer(const QString &name);
  QJsonArray clientsStatus() const;

  QString m_settingsPath;
  QString m_trustedDbOverride;
  QString m_stateDirectory;
  std::unique_ptr<QSettings> m_settings;
  std::unique_ptr<QSettings> m_state; //!< change times and clients' settings, ours alone
  QFileSystemWatcher m_watcher;
  QTimer m_reloadTimer;
  QTimer m_tickTimer;
  Config m_config;
  std::unique_ptr<AudioPeer> m_peer;
  QElapsedTimer m_clock;

  // Client.
  shared::Values m_local;
  std::map<QString, qint64> m_appliedAtMs; //!< when the server last changed each setting
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
  std::map<QString, ClientRecord> m_clients;
  std::set<int> m_microphoneLinks; //!< clients asking for the microphone
  std::optional<bool> m_sentSystemDemand;
  bool m_playbackFailing = false;
  QString m_playbackError;
  qint64 m_lastPlaybackAttemptMs = 0;
  qint64 m_playbackIdleSinceMs = 0;
  bool m_wantRecording = false;
  QString m_recordError;
  qint64 m_lastRecordAttemptMs = 0;

  Watchdog m_captureWatch;
  Watchdog m_feedWatch;
  Watchdog m_playbackWatch;
  Watchdog m_recordWatch;

  // Declared before the devices, whose callbacks use them.
  Slot<SendStream> m_systemSend;
  Slot<std::vector<std::shared_ptr<SendStream>>> m_microphoneSends;
  Slot<ReceiveStream> m_microphoneReceive;
  PlaybackMixer m_mixer;
#if defined(Q_OS_MACOS)
  std::unique_ptr<macos::SystemAudioCapture> m_capture;
  std::unique_ptr<macos::VirtualMicrophone> m_microphone;
#endif
  std::unique_ptr<AudioDevice> m_playback;
  std::unique_ptr<AudioDevice> m_recorder;
};

} // namespace deskflow::audio
