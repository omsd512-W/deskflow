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
  connect(&m_pingTimer, &QTimer::timeout, this, qOverload<>(&AudioPeer::sendPing));
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
        if (int(m_links.size()) >= kMaxLinks) {
          log(QStringLiteral("turning away %1, already serving %2 computers")
                  .arg(socket->peerAddress().toString())
                  .arg(kMaxLinks));
          socket->abort();
          socket->deleteLater();
          continue;
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
  // Closing a channel removes its link, so close a copy.
  std::vector<TlsChannel *> channels;
  for (const auto &[id, link] : m_links) {
    if (link->channel) {
      channels.push_back(link->channel);
    }
  }
  for (auto *channel : channels) {
    channel->close(QStringLiteral("stopping"));
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
    m_links.clear();
  }
  if (m_udp) {
    m_udp->close();
  }
}

std::shared_ptr<AudioPeer::Link> AudioPeer::link(int id) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_links.find(id);
  return it == m_links.end() ? nullptr : it->second;
}

std::shared_ptr<AudioPeer::Link> AudioPeer::firstLink() const
{
  std::lock_guard lock(m_mutex);
  for (const auto &[id, link] : m_links) {
    if (link->connected) {
      return link;
    }
  }
  return nullptr;
}

std::vector<int> AudioPeer::links() const
{
  std::vector<int> ids;
  std::lock_guard lock(m_mutex);
  for (const auto &[id, link] : m_links) {
    if (link->connected) {
      ids.push_back(id);
    }
  }
  return ids;
}

QString AudioPeer::peerName() const
{
  const auto first = firstLink();
  return first ? first->name : QString();
}

QString AudioPeer::peerName(int id) const
{
  const auto found = link(id);
  return found ? found->name : QString();
}

int AudioPeer::linkNamed(const QString &name) const
{
  std::lock_guard lock(m_mutex);
  for (const auto &[id, link] : m_links) {
    if (link->connected && link->name == name) {
      return id;
    }
  }
  return -1;
}

double AudioPeer::roundTripMs() const
{
  const auto first = firstLink();
  return first ? first->roundTripMs : 0;
}

double AudioPeer::roundTripMs(int id) const
{
  const auto found = link(id);
  return found ? found->roundTripMs : 0;
}

std::optional<std::int64_t> AudioPeer::clockOffsetUs() const
{
  const auto first = firstLink();
  return first ? first->clockOffsetUs : std::nullopt;
}

std::optional<std::int64_t> AudioPeer::clockOffsetUs(int id) const
{
  const auto found = link(id);
  return found ? found->clockOffsetUs : std::nullopt;
}

void AudioPeer::setSending(StreamKind kind, bool enabled)
{
  if (enabled) {
    m_sending.insert(kind);
  } else {
    m_sending.erase(kind);
  }
  for (const auto id : links()) {
    setSending(id, kind, enabled);
  }
}

void AudioPeer::setSending(int id, StreamKind kind, bool enabled)
{
  const auto found = link(id);
  if (!found) {
    return;
  }
  if (enabled) {
    found->sending.insert(kind);
    if (found->connected && !sendStream(id, kind)) {
      openSendStream(*found, kind);
    }
  } else {
    found->sending.erase(kind);
    if (found->connected) {
      closeSendStream(*found, kind);
    }
  }
}

std::shared_ptr<SendStream> AudioPeer::sendStream(StreamKind kind) const
{
  const auto first = firstLink();
  return first ? sendStream(first->id, kind) : nullptr;
}

std::shared_ptr<SendStream> AudioPeer::sendStream(int id, StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_links.find(id);
  if (it == m_links.end()) {
    return nullptr;
  }
  const auto stream = it->second->send.find(kind);
  return stream == it->second->send.end() ? nullptr : stream->second;
}

std::shared_ptr<ReceiveStream> AudioPeer::receiveStream(StreamKind kind) const
{
  const auto first = firstLink();
  return first ? receiveStream(first->id, kind) : nullptr;
}

std::shared_ptr<ReceiveStream> AudioPeer::receiveStream(int id, StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_links.find(id);
  if (it == m_links.end()) {
    return nullptr;
  }
  for (const auto &[streamId, incoming] : it->second->incoming) {
    if (incoming->kind == kind) {
      return incoming->stream;
    }
  }
  return nullptr;
}

QJsonObject AudioPeer::remoteStats(StreamKind kind) const
{
  const auto first = firstLink();
  return first ? remoteStats(first->id, kind) : QJsonObject{};
}

QJsonObject AudioPeer::remoteStats(int id, StreamKind kind) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_links.find(id);
  if (it == m_links.end()) {
    return {};
  }
  const auto stats = it->second->remoteStats.find(kind);
  return stats == it->second->remoteStats.end() ? QJsonObject{} : stats->second;
}

void AudioPeer::sendDemand(StreamKind kind, bool active)
{
  for (const auto id : links()) {
    sendDemand(id, kind, active);
  }
}

void AudioPeer::sendDemand(int id, StreamKind kind, bool active)
{
  sendMessage(id, protocol::demand(kind, active));
}

void AudioPeer::sendMessage(int id, const QJsonObject &message)
{
  const auto found = link(id);
  if (found && found->connected && found->channel) {
    found->channel->send(message);
  }
}

AudioPeer::DatagramCounters AudioPeer::datagramCounters() const
{
  std::lock_guard lock(m_mutex);
  return m_counters;
}

void AudioPeer::connectToServer()
{
  if (!m_links.empty() || m_connecting) {
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
  auto added = std::make_shared<Link>();
  added->id = m_nextLinkId++;
  added->sending = m_sending;
  added->channel = new TlsChannel(socket, server ? TlsChannel::Mode::Server : TlsChannel::Mode::Client, this);
  const int id = added->id;
  auto *channel = added->channel;
  {
    std::lock_guard lock(m_mutex);
    m_links[id] = added;
  }
  connect(channel, &TlsChannel::ready, this, [this, id] { onChannelReady(id); });
  connect(channel, &TlsChannel::messageReceived, this, [this, id](const QJsonObject &message) {
    onMessage(id, message);
  });
  connect(channel, &TlsChannel::closed, this, [this, id](const QString &reason) { onChannelClosed(id, reason); });
  connect(channel, &TlsChannel::closed, channel, &QObject::deleteLater);
  QTimer::singleShot(kHandshakeTimeoutMs, channel, [this, id, channel] {
    const auto found = link(id);
    if (found && found->channel == channel && !found->connected) {
      channel->close(QStringLiteral("handshake timed out"));
    }
  });
  QString error;
  if (!channel->start(m_config.certificatePath, &error)) {
    log(error);
    channel->close(error);
  }
}

void AudioPeer::onChannelReady(int id)
{
  const auto found = link(id);
  if (!found || !found->channel) {
    return;
  }
  const auto digest = found->channel->peerSha256();
  if (!isTrusted(digest)) {
    log(QStringLiteral("rejecting peer with untrusted certificate %1").arg(deskflow::formatSSLFingerprint(digest)));
    found->channel->close(QStringLiteral("untrusted peer"));
    return;
  }
  found->channel->send(protocol::hello(m_config.name, m_udp->localPort()));
}

void AudioPeer::onHello(Link &link, const QJsonObject &message)
{
  const auto hello = protocol::parseHello(message);
  if (!hello || hello->version != protocol::kVersion) {
    link.channel->close(QStringLiteral("incompatible peer"));
    return;
  }
  // The same computer coming back (after sleep, say) replaces its old link.
  if (m_config.role == PeerConfig::Role::Server && !hello->name.isEmpty()) {
    std::vector<TlsChannel *> stale;
    for (const auto &[id, other] : m_links) {
      if (id != link.id && other->connected && other->name == hello->name && other->channel) {
        stale.push_back(other->channel);
      }
    }
    for (auto *channel : stale) {
      log(QStringLiteral("%1 connected again, dropping its old connection").arg(hello->name));
      channel->close(QStringLiteral("replaced"));
    }
  }
  link.name = hello->name;
  link.address = link.channel->socket()->peerAddress();
  link.udpPort = hello->udpPort;
  link.connected = true;
  log(QStringLiteral("connected to %1 (%2, UDP %3)").arg(link.name, link.address.toString()).arg(link.udpPort));
  for (const auto kind : link.sending) {
    openSendStream(link, kind);
  }
  link.clockSamples.clear();
  link.clockOffsetUs.reset();
  if (!m_pingTimer.isActive()) {
    m_pingTimer.start();
    m_statsTimer.start();
  }
  // A quick burst gives a usable clock offset within a second.
  const int id = link.id;
  for (int i = 0; i < 5; ++i) {
    QTimer::singleShot(i * 100, this, [this, id] {
      if (const auto found = this->link(id); found && found->connected) {
        sendPing(*found);
      }
    });
  }
  Q_EMIT linkConnected(id);
  Q_EMIT connected();
}

void AudioPeer::onMessage(int id, const QJsonObject &message)
{
  const auto found = link(id);
  if (!found || !found->channel) {
    return;
  }
  auto &link = *found;
  link.lastHeard.restart();
  const auto type = message.value(protocol::kType).toString();

  if (type == protocol::kHello) {
    onHello(link, message);
    return;
  }

  if (!link.connected) {
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
      for (auto it = link.incoming.begin(); it != link.incoming.end();) {
        it = (it->second->kind == incoming->kind) ? link.incoming.erase(it) : std::next(it);
      }
      link.incoming[offer->id] = incoming;
    }
    log(QStringLiteral("receiving %1 audio from %2: %3 ch, %4 ms frames")
            .arg(QString::fromLatin1(toString(offer->format.kind)), link.name)
            .arg(offer->format.channels)
            .arg(offer->format.frameMs()));
    Q_EMIT receiveStreamChanged(id, offer->format.kind);
  } else if (type == protocol::kStreamClose) {
    const auto streamId = std::uint8_t(message.value("id").toInt());
    std::optional<StreamKind> kind;
    {
      std::lock_guard lock(m_mutex);
      const auto it = link.incoming.find(streamId);
      if (it != link.incoming.end()) {
        kind = it->second->kind;
        link.incoming.erase(it);
      }
    }
    if (kind) {
      Q_EMIT receiveStreamChanged(id, *kind);
    }
  } else if (type == protocol::kDemand) {
    const auto kind = protocol::kindFromString(message.value("kind").toString());
    if (kind) {
      Q_EMIT demandChanged(id, *kind, message.value("active").toBool());
    }
  } else if (type == protocol::kPing) {
    link.channel->send(protocol::pong(std::int64_t(message.value("t").toDouble()), toMicros(Clock::now())));
  } else if (type == protocol::kPong) {
    onPong(link, message);
  } else if (type == protocol::kTiming) {
    const auto streamId = std::uint8_t(message.value("id").toInt());
    std::lock_guard lock(m_mutex);
    if (const auto it = link.incoming.find(streamId); it != link.incoming.end()) {
      it->second->mark = SendStream::CaptureMark{
          std::uint32_t(message.value("ts").toDouble()), std::int64_t(message.value("us").toDouble())
      };
    }
  } else if (type == protocol::kStats) {
    const auto streamId = std::uint8_t(message.value("id").toInt());
    std::lock_guard lock(m_mutex);
    for (const auto &[kind, stream] : link.send) {
      if (stream->id() == streamId) {
        link.remoteStats[kind] = message;
      }
    }
  } else {
    Q_EMIT messageReceived(id, message);
  }
}

void AudioPeer::onChannelClosed(int id, const QString &reason)
{
  std::shared_ptr<Link> removed;
  {
    std::lock_guard lock(m_mutex);
    const auto it = m_links.find(id);
    if (it == m_links.end()) {
      return;
    }
    removed = it->second;
    m_links.erase(it);
  }
  removed->channel = nullptr;
  std::set<StreamKind> kinds;
  for (const auto &[streamId, incoming] : removed->incoming) {
    kinds.insert(incoming->kind);
  }
  for (const auto kind : kinds) {
    Q_EMIT receiveStreamChanged(id, kind);
  }
  if (m_links.empty()) {
    m_pingTimer.stop();
    m_statsTimer.stop();
  }
  // audio-soak-report.py counts lines starting "disconnected: ".
  log(removed->name.isEmpty() ? QStringLiteral("disconnected: %1").arg(reason)
                              : QStringLiteral("disconnected: %1 (%2)").arg(reason, removed->name));
  if (removed->connected) {
    Q_EMIT linkDisconnected(id, reason);
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

void AudioPeer::openSendStream(Link &link, StreamKind kind)
{
  const auto format = defaultFormat(kind);
  const auto id = std::uint8_t(kind);
  const auto key = StreamKey::generate();
  SendStream::Transmit transmit = [udp = m_udp, address = link.address,
                                   port = link.udpPort](const std::uint8_t *data, int size) {
    udp->sendTo(data, size, address, port);
  };
  if (m_config.wrapTransmit) {
    transmit = m_config.wrapTransmit(std::move(transmit));
  }
  auto stream = std::make_shared<SendStream>(format, id, key, std::move(transmit));
  {
    std::lock_guard lock(m_mutex);
    link.send[kind] = stream;
  }
  link.channel->send(protocol::streamOffer(id, format, key));
  log(QStringLiteral("sending %1 audio to %2").arg(QString::fromLatin1(toString(kind)), link.name));
}

void AudioPeer::closeSendStream(Link &link, StreamKind kind)
{
  std::shared_ptr<SendStream> stream;
  {
    std::lock_guard lock(m_mutex);
    const auto it = link.send.find(kind);
    if (it == link.send.end()) {
      return;
    }
    stream = it->second;
    link.send.erase(it);
    link.remoteStats.erase(kind);
  }
  if (link.channel) {
    link.channel->send(protocol::streamClose(stream->id()));
  }
}

void AudioPeer::sendStats()
{
  for (const auto id : links()) {
    const auto found = link(id);
    if (!found || !found->channel) {
      continue;
    }
    std::vector<std::pair<std::uint8_t, std::shared_ptr<ReceiveStream>>> streams;
    std::vector<std::shared_ptr<SendStream>> sending;
    {
      std::lock_guard lock(m_mutex);
      for (const auto &[streamId, incoming] : found->incoming) {
        streams.emplace_back(streamId, incoming->stream);
      }
      for (const auto &[kind, stream] : found->send) {
        sending.push_back(stream);
      }
    }
    for (const auto &[streamId, stream] : streams) {
      auto message = protocol::stats(streamId, stream->stats());
      if (const auto latency = latencyMs(id, stream->format().kind)) {
        message.insert(QStringLiteral("latencyMs"), *latency);
      }
      found->channel->send(message);
    }
    for (const auto &stream : sending) {
      if (const auto mark = stream->captureMark()) {
        found->channel->send(protocol::timing(stream->id(), mark->timestamp, mark->captureUs));
      }
    }
  }
}

void AudioPeer::sendPing()
{
  std::vector<TlsChannel *> silent;
  for (const auto id : links()) {
    const auto found = link(id);
    if (!found || !found->channel) {
      continue;
    }
    if (found->lastHeard.isValid() && found->lastHeard.elapsed() > kSilenceTimeoutMs) {
      silent.push_back(found->channel);
      continue;
    }
    sendPing(*found);
  }
  for (auto *channel : silent) {
    channel->close(QStringLiteral("no reply for %1 s").arg(kSilenceTimeoutMs / 1000));
  }
}

void AudioPeer::sendPing(Link &link)
{
  if (link.channel && link.connected) {
    link.channel->send(protocol::ping(toMicros(Clock::now())));
  }
}

void AudioPeer::onPong(Link &link, const QJsonObject &message)
{
  // NTP style: the responder's clock read halfway through the round trip.
  const auto sent = std::int64_t(message.value("t").toDouble());
  const auto peer = std::int64_t(message.value("r").toDouble());
  const auto now = toMicros(Clock::now());
  const auto roundTrip = now - sent;
  if (roundTrip < 0) {
    return;
  }
  link.roundTripMs = roundTrip / 1000.0;
  link.clockSamples.push_back({roundTrip, peer - (sent + now) / 2});
  while (link.clockSamples.size() > 16) {
    link.clockSamples.pop_front();
  }
  // The quickest exchange has the least room for asymmetric delay.
  const auto best =
      std::min_element(link.clockSamples.begin(), link.clockSamples.end(), [](const auto &a, const auto &b) {
        return a.roundTripUs < b.roundTripUs;
      });
  link.clockOffsetUs = best->offsetUs;
}

void AudioPeer::setOutputLatency(StreamKind kind, double ms)
{
  std::lock_guard lock(m_mutex);
  m_outputLatencyMs[kind] = ms;
}

std::optional<double> AudioPeer::latencyMs(StreamKind kind) const
{
  const auto first = firstLink();
  return first ? latencyMs(first->id, kind) : std::nullopt;
}

std::optional<double> AudioPeer::latencyMs(int id, StreamKind kind) const
{
  const auto found = link(id);
  if (!found || !found->clockOffsetUs) {
    return std::nullopt;
  }
  const auto offset = *found->clockOffsetUs;
  std::shared_ptr<ReceiveStream> stream;
  std::optional<SendStream::CaptureMark> mark;
  double outputMs = 0;
  {
    std::lock_guard lock(m_mutex);
    for (const auto &[streamId, incoming] : found->incoming) {
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
  const double capturedUs = double(mark->captureUs - offset) + sinceMark * 1e6 / kSampleRate;
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
    // Every computer numbers its streams the same way, so the stream id alone
    // does not say whose packet this is; only that link's key opens it.
    bool known = false;
    bool stale = false;
    bool opened = false;
    for (const auto &[id, link] : m_links) {
      const auto it = link->incoming.find(header->streamId);
      if (it == link->incoming.end()) {
        continue;
      }
      known = true;
      auto &incoming = *it->second;
      if (!incoming.replay.isFresh(header->sequence)) {
        stale = true;
        continue;
      }
      MediaHeader result;
      const int length = incoming.opener->open(datagram, size, result, payload, sizeof(payload));
      if (length < 0) {
        continue;
      }
      incoming.replay.accept(result.sequence);
      incoming.stream->push(result, payload, length, arrival);
      opened = true;
      break;
    }
    if (!known) {
      ++m_counters.unknownStream;
    } else if (!opened) {
      ++(stale ? m_counters.replayed : m_counters.forged);
    }
  }
}

void AudioPeer::log(const QString &line)
{
  Q_EMIT logMessage(line);
}

} // namespace deskflow::audio
