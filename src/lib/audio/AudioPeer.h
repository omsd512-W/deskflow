/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "audio/AudioTypes.h"
#include "audio/MediaPacket.h"
#include "audio/ReceiveStream.h"
#include "audio/SendStream.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <deque>
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

class QTcpServer;
class QTcpSocket;

namespace deskflow::audio {

class TlsChannel;
class UdpSocket;

struct PeerConfig
{
  enum class Role
  {
    Server,
    Client
  };

  Role role = Role::Server;
  QString host;          //!< client only: the server's address
  quint16 port = 24801;  //!< server: TCP and UDP port to listen on; client: server's TCP port
  QString certificatePath; //!< PEM certificate and key, normally Deskflow's own
  QString trustedDbPath;   //!< Deskflow fingerprint database of peers to accept
  QList<QByteArray> trustedSha256; //!< extra raw SHA-256 digests to accept
  QString name;
  //! Wraps each stream's transmit function, e.g. to simulate a bad network.
  std::function<SendStream::Transmit(SendStream::Transmit)> wrapTransmit;
};

/**
 * One end of an audio link between two Deskflow computers: a TLS control
 * connection (server listens, client connects and reconnects) plus one UDP
 * socket carrying every stream in both directions.
 *
 * Lives on the thread that runs its event loop. sendStream() and
 * receiveStream() hand out shared pointers that capture and playback threads
 * may keep using; a new pair is created on every (re)connect.
 */
class AudioPeer : public QObject
{
  Q_OBJECT
public:
  explicit AudioPeer(PeerConfig config, QObject *parent = nullptr);
  ~AudioPeer() override;

  bool start(QString *error = nullptr);
  void stop();

  bool isConnected() const
  {
    return m_connected;
  }
  QString peerName() const
  {
    return m_peerName;
  }
  double roundTripMs() const
  {
    return m_roundTripMs.load(std::memory_order_relaxed);
  }
  //! Peer clock minus ours (µs), from the quickest recent ping; nullopt until measured.
  std::optional<std::int64_t> clockOffsetUs() const;

  //! How long after capture on the peer the stream of this kind is heard
  //! here, given how long the local output device takes to play what it is
  //! handed. nullopt until the clocks are synchronised and audio is playing.
  std::optional<double> latencyMs(StreamKind kind) const;
  //! Set by whoever plays the stream of this kind (any thread).
  void setOutputLatency(StreamKind kind, double ms);

  //! Offer a stream of this kind whenever connected.
  void setSending(StreamKind kind, bool enabled);
  std::shared_ptr<SendStream> sendStream(StreamKind kind) const;
  std::shared_ptr<ReceiveStream> receiveStream(StreamKind kind) const;
  //! The peer's latest report about the stream of this kind that we send.
  QJsonObject remoteStats(StreamKind kind) const;

  //! Tell the peer whether we currently need its stream of this kind.
  void sendDemand(StreamKind kind, bool active);

  struct DatagramCounters
  {
    std::uint64_t received = 0;
    std::uint64_t unknownStream = 0;
    std::uint64_t replayed = 0;
    std::uint64_t forged = 0;
  };
  DatagramCounters datagramCounters() const;

Q_SIGNALS:
  void connected();
  void disconnected(const QString &reason);
  void receiveStreamChanged(deskflow::audio::StreamKind kind);
  void demandChanged(deskflow::audio::StreamKind kind, bool active);
  void logMessage(const QString &line);

private:
  struct Incoming
  {
    std::uint8_t id = 0;
    StreamKind kind = StreamKind::System;
    std::shared_ptr<ReceiveStream> stream;
    std::unique_ptr<PacketOpener> opener;
    ReplayWindow replay;
    std::optional<SendStream::CaptureMark> mark; //!< guarded by m_mutex
  };

  void connectToServer();
  void scheduleReconnect();
  void attachChannel(QTcpSocket *socket, bool server);
  void onChannelReady();
  void onMessage(const QJsonObject &message);
  void onChannelClosed(const QString &reason);
  bool isTrusted(const QByteArray &sha256) const;
  void openSendStream(StreamKind kind);
  void closeSendStream(StreamKind kind);
  void sendStats();
  void sendPing();
  void onPong(const QJsonObject &message);
  void receiveLoop();
  void log(const QString &line);

  PeerConfig m_config;
  QTcpServer *m_server = nullptr;
  QTcpSocket *m_connecting = nullptr;
  TlsChannel *m_channel = nullptr;
  QTimer m_reconnectTimer;
  QTimer m_pingTimer;
  QTimer m_statsTimer;
  int m_reconnectDelayMs = 1000;

  bool m_connected = false;
  QString m_peerName;
  QHostAddress m_remoteAddress;
  quint16 m_remoteUdpPort = 0;
  QElapsedTimer m_lastHeard; //!< since the last control message from the peer
  std::atomic<double> m_roundTripMs{0};
  struct ClockSample
  {
    std::int64_t roundTripUs;
    std::int64_t offsetUs;
  };
  std::deque<ClockSample> m_clockSamples; // main thread
  std::atomic<std::int64_t> m_clockOffsetUs{0};
  std::atomic<bool> m_haveClockOffset{false};

  std::shared_ptr<UdpSocket> m_udp;
  std::thread m_receiver;
  std::atomic<bool> m_running{false};

  std::set<StreamKind> m_sending;
  mutable std::mutex m_mutex; // guards the four members below
  std::map<StreamKind, std::shared_ptr<SendStream>> m_send;
  std::map<std::uint8_t, std::shared_ptr<Incoming>> m_incoming;
  std::map<StreamKind, QJsonObject> m_remoteStats;
  std::map<StreamKind, double> m_outputLatencyMs;
  DatagramCounters m_counters;
};

} // namespace deskflow::audio
