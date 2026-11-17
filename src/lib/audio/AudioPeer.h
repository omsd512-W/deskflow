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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

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
 * One end of the audio links between Deskflow computers: TLS control
 * connections (the server listens and takes several clients, a client
 * connects to one server and reconnects) plus one UDP socket carrying every
 * stream in both directions.
 *
 * Each connected computer is a link with its own id. The methods without a
 * link id act on the first connected link, which is all a client has.
 *
 * Lives on the thread that runs its event loop. sendStream() and
 * receiveStream() hand out shared pointers that capture and playback threads
 * may keep using; a new pair is created on every (re)connect.
 */
class AudioPeer : public QObject
{
  Q_OBJECT
public:
  //! How many computers a server takes at once.
  static constexpr int kMaxLinks = 8;

  explicit AudioPeer(PeerConfig config, QObject *parent = nullptr);
  ~AudioPeer() override;

  bool start(QString *error = nullptr);
  void stop();

  //! Connected links, oldest first.
  std::vector<int> links() const;
  bool isConnected() const
  {
    return !links().empty();
  }
  QString peerName() const;
  QString peerName(int link) const;
  //! The connected link to this computer, or -1.
  int linkNamed(const QString &name) const;

  double roundTripMs() const;
  double roundTripMs(int link) const;
  //! Peer clock minus ours (µs), from the quickest recent ping; nullopt until measured.
  std::optional<std::int64_t> clockOffsetUs() const;
  std::optional<std::int64_t> clockOffsetUs(int link) const;

  //! How long after capture on the peer the stream of this kind is heard
  //! here, given how long the local output device takes to play what it is
  //! handed. nullopt until the clocks are synchronised and audio is playing.
  std::optional<double> latencyMs(StreamKind kind) const;
  std::optional<double> latencyMs(int link, StreamKind kind) const;
  //! Set by whoever plays the stream of this kind (any thread).
  void setOutputLatency(StreamKind kind, double ms);

  //! Offer a stream of this kind to every link, now and whenever one connects.
  void setSending(StreamKind kind, bool enabled);
  //! Offer a stream of this kind to one link only.
  void setSending(int link, StreamKind kind, bool enabled);
  std::shared_ptr<SendStream> sendStream(StreamKind kind) const;
  std::shared_ptr<SendStream> sendStream(int link, StreamKind kind) const;
  std::shared_ptr<ReceiveStream> receiveStream(StreamKind kind) const;
  std::shared_ptr<ReceiveStream> receiveStream(int link, StreamKind kind) const;
  //! The peer's latest report about the stream of this kind that we send.
  QJsonObject remoteStats(StreamKind kind) const;
  QJsonObject remoteStats(int link, StreamKind kind) const;

  //! Tell every link whether we currently need its stream of this kind.
  void sendDemand(StreamKind kind, bool active);
  void sendDemand(int link, StreamKind kind, bool active);
  //! Send a control message of a type this class does not handle itself.
  void sendMessage(int link, const QJsonObject &message);

  struct DatagramCounters
  {
    std::uint64_t received = 0;
    std::uint64_t unknownStream = 0;
    std::uint64_t replayed = 0;
    std::uint64_t forged = 0;
  };
  DatagramCounters datagramCounters() const;

Q_SIGNALS:
  //! A link finished its handshake (connected) or went away (disconnected).
  void connected();
  void disconnected(const QString &reason);
  void linkConnected(int link);
  void linkDisconnected(int link, const QString &reason);
  void receiveStreamChanged(int link, deskflow::audio::StreamKind kind);
  void demandChanged(int link, deskflow::audio::StreamKind kind, bool active);
  //! A control message of a type this class does not handle itself.
  void messageReceived(int link, const QJsonObject &message);
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

  struct ClockSample
  {
    std::int64_t roundTripUs;
    std::int64_t offsetUs;
  };

  //! One connection. Fields marked main are only touched on the main thread.
  struct Link
  {
    int id = 0;
    TlsChannel *channel = nullptr;             // main
    bool connected = false;                    // main
    QString name;                              // main
    QHostAddress address;                      // main
    quint16 udpPort = 0;                       // main
    QElapsedTimer lastHeard;                   // main
    double roundTripMs = 0;                    // main
    std::deque<ClockSample> clockSamples;      // main
    std::optional<std::int64_t> clockOffsetUs; // main
    std::set<StreamKind> sending;              // main
    // Guarded by AudioPeer::m_mutex.
    std::map<StreamKind, std::shared_ptr<SendStream>> send;
    std::map<std::uint8_t, std::shared_ptr<Incoming>> incoming;
    std::map<StreamKind, QJsonObject> remoteStats;
  };

  std::shared_ptr<Link> link(int id) const;
  std::shared_ptr<Link> firstLink() const;
  void connectToServer();
  void scheduleReconnect();
  void attachChannel(QTcpSocket *socket, bool server);
  void onChannelReady(int id);
  void onHello(Link &link, const QJsonObject &message);
  void onMessage(int id, const QJsonObject &message);
  void onChannelClosed(int id, const QString &reason);
  bool isTrusted(const QByteArray &sha256) const;
  void openSendStream(Link &link, StreamKind kind);
  void closeSendStream(Link &link, StreamKind kind);
  void sendStats();
  void sendPing();
  void sendPing(Link &link);
  void onPong(Link &link, const QJsonObject &message);
  void receiveLoop();
  void log(const QString &line);

  PeerConfig m_config;
  QTcpServer *m_server = nullptr;
  QTcpSocket *m_connecting = nullptr;
  QTimer m_reconnectTimer;
  QTimer m_pingTimer;
  QTimer m_statsTimer;
  int m_reconnectDelayMs = 1000;
  int m_nextLinkId = 1;

  std::shared_ptr<UdpSocket> m_udp;
  std::thread m_receiver;
  std::atomic<bool> m_running{false};

  std::set<StreamKind> m_sending;               //!< offered to every link
  mutable std::mutex m_mutex;                   // guards the links' streams and the members below
  std::map<int, std::shared_ptr<Link>> m_links; // added and removed on the main thread
  std::map<StreamKind, double> m_outputLatencyMs;
  DatagramCounters m_counters;
};

} // namespace deskflow::audio
