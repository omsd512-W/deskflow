/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/AudioPeer.h"

#include "audio/ControlProtocol.h"
#include "audio/TlsChannel.h"
#include "audio/UdpSocket.h"

#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"

#include <QTcpServer>
#include <QTcpSocket>

namespace deskflow::audio {

namespace {
constexpr int kMaxReconnectDelayMs = 5000;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeTimeoutMs = 10000;
//! A connected peer answers pings every second; this much silence means it is gone
//! (asleep, unplugged, crashed) even if TCP has not noticed yet.
constexpr int kSilenceTimeoutMs = 4000;
} // namespace

AudioPeer::AudioPeer(PeerConfig config, QObject *parent) : QObject(parent), m_config(std::move(config))
{
  m_reconnectTimer.setSingleShot(true);
  connect(&m_reconnectTimer, &QTimer::timeout, this, &AudioPeer::connectToServer);
  m_pingTimer.setInterval(1000);
  connect(&m_pingTimer, &QTimer::timeout, this, &AudioPeer::sendPing);
  m_statsTimer.setInterval(1000);
  connect(&m_statsTimer, &QTimer::timeout, this, &AudioPeer::sendStats);
}

AudioPeer::~AudioPeer()
{
  stop();
}

bool AudioPeer::start(QString *error)
{
  m_udp = std::make_shared<UdpSocket>();
  const quint16 udpPort = m_config.role == PeerConfig::Role::Server ? m_config.port : 0;
  if (!m_udp->bind(udpPort, error)) {
    return false;
  }
  m_running = true;
  m_receiver = std::thread([this] { receiveLoop(); });

  if (m_config.role == PeerConfig::Role::Server) {
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = m_server->nextPendingConnection()) {
        if (m_channel) {
          log(QStringLiteral("a new client replaces the current connection"));
          m_channel->close(QStringLiteral("replaced"));
        }
        attachChannel(socket, true);
      }
    });
    if (!m_server->listen(QHostAddress::Any, m_config.port)) {
      if (error) {
        *error = QStringLiteral("cannot listen on TCP port %1: %2").arg(m_config.port).arg(m_server->errorString());
      }
      stop();
      return false;
    }
    log(QStringLiteral("listening on port %1 (TCP control, UDP media)").arg(m_config.port));
  } else {
    log(QStringLiteral("UDP media on local port %1").arg(m_udp->localPort()));
    connectToServer();
  }
  return true;
}

void AudioPeer::stop()
{
  m_reconnectTimer.stop();
  m_pingTimer.stop();
  m_statsTimer.stop();
  if (m_channel) {
    m_channel->close(QStringLiteral("stopping"));
  }
  if (m_server) {
    m_server->close();
  }
  m_running = false;
  if (m_receiver.joinable()) {
    m_receiver.join();
  }
  {
    std::lock_guard lock(m_mutex);
    m_send.clear();
    m_incoming.clear();
  }
  if (m_udp) {
    m_udp->close();
  }
}

void AudioPeer::setSending(StreamKind kind, bool enabled)
{
  if (enabled) {
    m_sending.insert(kind);
    if (m_connected && !sendStream(kind)) {
      openSendStream(kind);
    }
  } else {
    m_sending.erase(kind);
    if (m_connected) {
      closeSendStream(kind);
    }
  }
}

std::shared_ptr<SendStream> AudioPeer::sendStream(StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_send.find(kind);
  return it == m_send.end() ? nullptr : it->second;
}

std::shared_ptr<ReceiveStream> AudioPeer::receiveStream(StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  for (const auto &[id, incoming] : m_incoming) {
    if (incoming->kind == kind) {
      return incoming->stream;
    }
  }
  return nullptr;
}

QJsonObject AudioPeer::remoteStats(StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_remoteStats.find(kind);
  return it == m_remoteStats.end() ? QJsonObject{} : it->second;
}

void AudioPeer::sendDemand(StreamKind kind, bool active)
{
  if (m_channel && m_connected) {
    m_channel->send(protocol::demand(kind, active));
  }
}

AudioPeer::DatagramCounters AudioPeer::datagramCounters() const
{
  std::lock_guard lock(m_mutex);
  return m_counters;
}

void AudioPeer::connectToServer()
{
  if (m_channel || m_connecting) {
    return;
  }
  m_connecting = new QTcpSocket(this);
  auto *socket = m_connecting;
  connect(socket, &QTcpSocket::connected, this, [this, socket] {
    m_connecting = nullptr;
    socket->disconnect(this);
    m_reconnectDelayMs = 1000;
    attachChannel(socket, false);
  });
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
    if (m_connecting != socket) {
      return;
    }
    log(QStringLiteral("cannot reach %1:%2: %3").arg(m_config.host).arg(m_config.port).arg(socket->errorString()));
    m_connecting = nullptr;
    socket->deleteLater();
    scheduleReconnect();
  });
  QTimer::singleShot(kConnectTimeoutMs, socket, [this, socket] {
    if (m_connecting != socket) {
      return;
    }
    log(QStringLiteral("cannot reach %1:%2: timed out").arg(m_config.host).arg(m_config.port));
    m_connecting = nullptr;
    socket->abort();
    socket->deleteLater();
    scheduleReconnect();
  });
  socket->connectToHost(m_config.host, m_config.port);
}

void AudioPeer::scheduleReconnect()
{
  if (m_config.role != PeerConfig::Role::Client || !m_running) {
    return;
  }
  m_reconnectTimer.start(m_reconnectDelayMs);
  m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
}

void AudioPeer::attachChannel(QTcpSocket *socket, bool server)
{
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  m_channel = new TlsChannel(socket, server ? TlsChannel::Mode::Server : TlsChannel::Mode::Client, this);
  auto *channel = m_channel;
  connect(channel, &TlsChannel::ready, this, &AudioPeer::onChannelReady);
  connect(channel, &TlsChannel::messageReceived, this, &AudioPeer::onMessage);
  connect(channel, &TlsChannel::closed, this, [this, channel](const QString &reason) {
    if (m_channel == channel) {
      onChannelClosed(reason);
    }
    channel->deleteLater();
  });
  QTimer::singleShot(kHandshakeTimeoutMs, channel, [this, channel] {
    if (m_channel == channel && !m_connected) {
      channel->close(QStringLiteral("handshake timed out"));
    }
  });
  QString error;
  if (!channel->start(m_config.certificatePath, &error)) {
    log(error);
    channel->close(error);
  }
}

void AudioPeer::onChannelReady()
{
  const auto digest = m_channel->peerSha256();
  if (!isTrusted(digest)) {
    log(QStringLiteral("rejecting peer with untrusted certificate %1").arg(deskflow::formatSSLFingerprint(digest)));
    m_channel->close(QStringLiteral("untrusted peer"));
    return;
  }
  m_channel->send(protocol::hello(m_config.name, m_udp->localPort()));
}

void AudioPeer::onMessage(const QJsonObject &message)
{
  m_lastHeard.restart();
  const auto type = message.value(protocol::kType).toString();

  if (type == protocol::kHello) {
    const auto hello = protocol::parseHello(message);
    if (!hello || hello->version != protocol::kVersion) {
      m_channel->close(QStringLiteral("incompatible peer"));
      return;
    }
    m_peerName = hello->name;
    m_remoteAddress = m_channel->socket()->peerAddress();
    m_remoteUdpPort = hello->udpPort;
    m_connected = true;
    log(QStringLiteral("connected to %1 (%2, UDP %3)")
            .arg(m_peerName, m_remoteAddress.toString())
            .arg(m_remoteUdpPort));
    for (const auto kind : m_sending) {
      openSendStream(kind);
    }
    m_clockSamples.clear();
    m_haveClockOffset = false;
    m_pingTimer.start();
    m_statsTimer.start();
    // A quick burst gives a usable clock offset within a second.
    for (int i = 0; i < 5; ++i) {
      QTimer::singleShot(i * 100, this, &AudioPeer::sendPing);
    }
    Q_EMIT connected();
    return;
  }

  if (!m_connected) {
    return;
  }

  if (type == protocol::kStreamOffer) {
    const auto offer = protocol::parseStreamOffer(message);
    if (!offer) {
      log(QStringLiteral("ignoring an invalid stream offer"));
      return;
    }
    auto incoming = std::make_shared<Incoming>();
    incoming->id = offer->id;
    incoming->kind = offer->format.kind;
    incoming->stream = std::make_shared<ReceiveStream>(offer->format);
    incoming->opener = std::make_unique<PacketOpener>(offer->key);
    {
      std::lock_guard lock(m_mutex);
      for (auto it = m_incoming.begin(); it != m_incoming.end();) {
        it = (it->second->kind == incoming->kind) ? m_incoming.erase(it) : std::next(it);
      }
      m_incoming[offer->id] = incoming;
    }
    log(QStringLiteral("receiving %1 audio: %2 ch, %3 ms frames")
            .arg(QString::fromLatin1(toString(offer->format.kind)))
            .arg(offer->format.channels)
            .arg(offer->format.frameMs()));
    Q_EMIT receiveStreamChanged(offer->format.kind);
  } else if (type == protocol::kStreamClose) {
    const auto id = std::uint8_t(message.value("id").toInt());
    std::optional<StreamKind> kind;
    {
      std::lock_guard lock(m_mutex);
      const auto it = m_incoming.find(id);
      if (it != m_incoming.end()) {
        kind = it->second->kind;
        m_incoming.erase(it);
      }
    }
    if (kind) {
      Q_EMIT receiveStreamChanged(*kind);
    }
  } else if (type == protocol::kDemand) {
    const auto kind = protocol::kindFromString(message.value("kind").toString());
    if (kind) {
      Q_EMIT demandChanged(*kind, message.value("active").toBool());
    }
  } else if (type == protocol::kPing) {
    m_channel->send(protocol::pong(std::int64_t(message.value("t").toDouble()), toMicros(Clock::now())));
  } else if (type == protocol::kPong) {
    onPong(message);
  } else if (type == protocol::kTiming) {
    const auto id = std::uint8_t(message.value("id").toInt());
    std::lock_guard lock(m_mutex);
    if (const auto it = m_incoming.find(id); it != m_incoming.end()) {
      it->second->mark = SendStream::CaptureMark{
          std::uint32_t(message.value("ts").toDouble()), std::int64_t(message.value("us").toDouble())
      };
    }
  } else if (type == protocol::kStats) {
    const auto id = std::uint8_t(message.value("id").toInt());
    std::lock_guard lock(m_mutex);
    for (const auto &[kind, stream] : m_send) {
      if (stream->id() == id) {
        m_remoteStats[kind] = message;
      }
    }
  }
}

void AudioPeer::onChannelClosed(const QString &reason)
{
  const bool wasConnected = m_connected;
  m_connected = false;
  m_channel = nullptr;
  m_pingTimer.stop();
  m_statsTimer.stop();
  std::set<StreamKind> removed;
  {
    std::lock_guard lock(m_mutex);
    m_send.clear();
    for (const auto &[id, incoming] : m_incoming) {
      removed.insert(incoming->kind);
    }
    m_incoming.clear();
    m_remoteStats.clear();
  }
  for (const auto kind : removed) {
    Q_EMIT receiveStreamChanged(kind);
  }
  log(QStringLiteral("disconnected: %1").arg(reason));
  if (wasConnected) {
    Q_EMIT disconnected(reason);
  }
  scheduleReconnect();
}

bool AudioPeer::isTrusted(const QByteArray &sha256) const
{
  if (sha256.isEmpty()) {
    return false;
  }
  if (m_config.trustedSha256.contains(sha256)) {
    return true;
  }
  if (m_config.trustedDbPath.isEmpty()) {
    return false;
  }
  FingerprintDatabase database;
  database.read(m_config.trustedDbPath);
  Fingerprint fingerprint;
  fingerprint.type = QCryptographicHash::Sha256;
  fingerprint.data = sha256;
  return database.isTrusted(fingerprint);
}

void AudioPeer::openSendStream(StreamKind kind)
{
  const auto format = defaultFormat(kind);
  const auto id = std::uint8_t(kind);
  const auto key = StreamKey::generate();
  SendStream::Transmit transmit = [udp = m_udp, address = m_remoteAddress,
                                   port = m_remoteUdpPort](const std::uint8_t *data, int size) {
    udp->sendTo(data, size, address, port);
  };
  if (m_config.wrapTransmit) {
    transmit = m_config.wrapTransmit(std::move(transmit));
  }
  auto stream = std::make_shared<SendStream>(format, id, key, std::move(transmit));
  {
    std::lock_guard lock(m_mutex);
    m_send[kind] = stream;
  }
  m_channel->send(protocol::streamOffer(id, format, key));
  log(QStringLiteral("sending %1 audio").arg(QString::fromLatin1(toString(kind))));
}

void AudioPeer::closeSendStream(StreamKind kind)
{
  std::shared_ptr<SendStream> stream;
  {
    std::lock_guard lock(m_mutex);
    const auto it = m_send.find(kind);
    if (it == m_send.end()) {
      return;
    }
    stream = it->second;
    m_send.erase(it);
    m_remoteStats.erase(kind);
  }
  if (m_channel) {
    m_channel->send(protocol::streamClose(stream->id()));
  }
}

void AudioPeer::sendStats()
{
  if (!m_channel) {
    return;
  }
  std::vector<std::pair<std::uint8_t, std::shared_ptr<ReceiveStream>>> streams;
  {
    std::lock_guard lock(m_mutex);
    for (const auto &[id, incoming] : m_incoming) {
      streams.emplace_back(id, incoming->stream);
    }
  }
  for (const auto &[id, stream] : streams) {
    auto message = protocol::stats(id, stream->stats());
    if (const auto latency = latencyMs(stream->format().kind)) {
      message.insert(QStringLiteral("latencyMs"), *latency);
    }
    m_channel->send(message);
  }
  std::vector<std::shared_ptr<SendStream>> sending;
  {
    std::lock_guard lock(m_mutex);
    for (const auto &[kind, stream] : m_send) {
      sending.push_back(stream);
    }
  }
  for (const auto &stream : sending) {
    if (const auto mark = stream->captureMark()) {
      m_channel->send(protocol::timing(stream->id(), mark->timestamp, mark->captureUs));
    }
  }
}

void AudioPeer::sendPing()
{
  if (m_channel && m_connected && m_lastHeard.isValid() && m_lastHeard.elapsed() > kSilenceTimeoutMs) {
    m_channel->close(QStringLiteral("no reply for %1 s").arg(m_lastHeard.elapsed() / 1000));
    return;
  }
  if (m_channel && m_connected) {
    m_channel->send(protocol::ping(toMicros(Clock::now())));
  }
}

void AudioPeer::onPong(const QJsonObject &message)
{
  // NTP style: the responder's clock read halfway through the round trip.
  const auto sent = std::int64_t(message.value("t").toDouble());
  const auto peer = std::int64_t(message.value("r").toDouble());
  const auto now = toMicros(Clock::now());
  const auto roundTrip = now - sent;
  if (roundTrip < 0) {
    return;
  }
  m_roundTripMs = roundTrip / 1000.0;
  m_clockSamples.push_back({roundTrip, peer - (sent + now) / 2});
  while (m_clockSamples.size() > 16) {
    m_clockSamples.pop_front();
  }
  // The quickest exchange has the least room for asymmetric delay.
  const auto best = std::min_element(m_clockSamples.begin(), m_clockSamples.end(), [](const auto &a, const auto &b) {
    return a.roundTripUs < b.roundTripUs;
  });
  m_clockOffsetUs = best->offsetUs;
  m_haveClockOffset = true;
}

std::optional<std::int64_t> AudioPeer::clockOffsetUs() const
{
  if (!m_haveClockOffset) {
    return std::nullopt;
  }
  return m_clockOffsetUs.load();
}

void AudioPeer::setOutputLatency(StreamKind kind, double ms)
{
  std::lock_guard lock(m_mutex);
  m_outputLatencyMs[kind] = ms;
}

std::optional<double> AudioPeer::latencyMs(StreamKind kind) const
{
  const auto offset = clockOffsetUs();
  if (!offset) {
    return std::nullopt;
  }
  std::shared_ptr<ReceiveStream> stream;
  std::optional<SendStream::CaptureMark> mark;
  double outputMs = 0;
  {
    std::lock_guard lock(m_mutex);
    for (const auto &[id, incoming] : m_incoming) {
      if (incoming->kind == kind) {
        stream = incoming->stream;
        mark = incoming->mark;
      }
    }
    if (const auto it = m_outputLatencyMs.find(kind); it != m_outputLatencyMs.end()) {
      outputMs = it->second;
    }
  }
  if (!stream || !mark) {
    return std::nullopt;
  }
  const auto stats = stream->stats();
  if (!stats.havePlayout) {
    return std::nullopt;
  }
  // When the sample now leaving was captured, in our clock.
  const auto sinceMark = std::int32_t(stats.playoutTimestamp - mark->timestamp);
  const double capturedUs = double(mark->captureUs - *offset) + sinceMark * 1e6 / kSampleRate;
  return (double(stats.playoutUs) - capturedUs) / 1000.0 + outputMs;
}

void AudioPeer::receiveLoop()
{
  std::uint8_t datagram[2048];
  std::uint8_t payload[kMaxPayloadBytes];
  while (m_running.load(std::memory_order_relaxed)) {
    const int size = m_udp->receive(datagram, sizeof(datagram), nullptr, nullptr, 200);
    if (size <= 0) {
      continue;
    }
    const auto arrival = Clock::now();
    const auto header = MediaHeader::read(datagram, std::size_t(size));
    if (!header || header->type != MediaHeader::Media) {
      continue;
    }
    std::lock_guard lock(m_mutex);
    ++m_counters.received;
    const auto it = m_incoming.find(header->streamId);
    if (it == m_incoming.end()) {
      ++m_counters.unknownStream;
      continue;
    }
    auto &incoming = *it->second;
    if (!incoming.replay.isFresh(header->sequence)) {
      ++m_counters.replayed;
      continue;
    }
    MediaHeader opened;
    const int length = incoming.opener->open(datagram, size, opened, payload, sizeof(payload));
    if (length < 0) {
      ++m_counters.forged;
      continue;
    }
    incoming.replay.accept(opened.sequence);
    incoming.stream->push(opened, payload, length, arrival);
  }
}

void AudioPeer::log(const QString &line)
{
  Q_EMIT logMessage(line);
}

} // namespace deskflow::audio
