/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioPeerTests.h"

#include "audio/AudioPeer.h"
#include "audio/UdpSocket.h"

#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>

#include <cmath>
#include <memory>
#include <numbers>

using namespace deskflow::audio;

namespace {

//! A port free for both UDP and TCP right now.
/*!
Picked at random below the ephemeral range. Windows reserves runs of hundreds of
ports for one protocol only (Hyper-V, WSL), and hands out ephemeral ports in
sequence, so every port it offers can fall in such a run. Claimed the way
AudioPeer claims it, UDP first and with the same socket.
*/
quint16 freePort()
{
  for (int attempt = 0; attempt < 50; ++attempt) {
    const auto port = static_cast<quint16>(QRandomGenerator::global()->bounded(20000, 49000));
    UdpSocket udp;
    if (!udp.bind(port)) {
      continue;
    }
    QTcpServer tcp;
    if (tcp.listen(QHostAddress::Any, port)) {
      return port;
    }
  }
  return 0;
}

PeerConfig serverConfig(quint16 port, const QString &cert, const QByteArray &trusted)
{
  PeerConfig config;
  config.role = PeerConfig::Role::Server;
  config.port = port;
  config.certificatePath = cert;
  config.trustedSha256 = {trusted};
  config.name = QStringLiteral("server");
  return config;
}

PeerConfig clientConfig(quint16 port, const QString &cert, const QByteArray &trusted)
{
  PeerConfig config;
  config.role = PeerConfig::Role::Client;
  config.host = QStringLiteral("127.0.0.1");
  config.port = port;
  config.certificatePath = cert;
  config.trustedSha256 = {trusted};
  config.name = QStringLiteral("client");
  return config;
}

//! Pushes 10 ms of tone into each send stream and pulls 10 ms from each receive stream, repeatedly.
void exchange(AudioPeer &a, AudioPeer &b, int iterations)
{
  std::vector<float> pcm(480 * 2);
  std::vector<float> out(480 * 2);
  double phase = 0;
  for (int i = 0; i < iterations; ++i) {
    for (int f = 0; f < 480; ++f) {
      pcm[std::size_t(f) * 2] = pcm[std::size_t(f) * 2 + 1] = float(0.25 * std::sin(phase));
      phase += 2 * std::numbers::pi * 440 / kSampleRate;
    }
    for (auto *peer : {&a, &b}) {
      for (auto kind : {StreamKind::System, StreamKind::Microphone}) {
        if (auto stream = peer->sendStream(kind)) {
          stream->push(pcm.data(), 480); // the mono stream reads the first half
        }
      }
    }
    QTest::qWait(10);
    for (auto *peer : {&a, &b}) {
      for (auto kind : {StreamKind::System, StreamKind::Microphone}) {
        if (auto stream = peer->receiveStream(kind)) {
          stream->pull(out.data(), 480);
        }
      }
    }
  }
}

} // namespace

void AudioPeerTests::initTestCase()
{
  QVERIFY(m_certificates.create());
}

void AudioPeerTests::test_streamsBothWays()
{
  const auto port = freePort();
  QVERIFY(port != 0);
  AudioPeer server(serverConfig(port, m_certificates.first, TestCertificates::sha256(m_certificates.second)));
  AudioPeer client(clientConfig(port, m_certificates.second, TestCertificates::sha256(m_certificates.first)));
  server.setSending(StreamKind::System, true);
  client.setSending(StreamKind::Microphone, true);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));
  QVERIFY2(client.start(&error), qPrintable(error));

  QTRY_VERIFY_WITH_TIMEOUT(server.isConnected() && client.isConnected(), 5000);
  QCOMPARE(client.peerName(), QStringLiteral("server"));
  QCOMPARE(server.peerName(), QStringLiteral("client"));
  QTRY_VERIFY(client.receiveStream(StreamKind::System) && server.receiveStream(StreamKind::Microphone));
  QVERIFY(!client.receiveStream(StreamKind::Microphone));
  QVERIFY(!server.receiveStream(StreamKind::System));

  exchange(server, client, 60);

  const auto system = client.receiveStream(StreamKind::System)->stats();
  const auto microphone = server.receiveStream(StreamKind::Microphone)->stats();
  QVERIFY2(system.packets > 40, qPrintable(QString::number(system.packets)));
  QVERIFY2(microphone.packets > 40, qPrintable(QString::number(microphone.packets)));
  QVERIFY(system.playing);
  QCOMPARE(client.datagramCounters().forged, 0u);
  QCOMPARE(server.datagramCounters().forged, 0u);
  QCOMPARE(client.datagramCounters().replayed, 0u);

  // Each side reports how the stream it receives is doing.
  QTRY_VERIFY_WITH_TIMEOUT(!server.remoteStats(StreamKind::System).isEmpty(), 3000);
  QVERIFY(server.remoteStats(StreamKind::System).value("packets").toDouble() > 0);
  QTRY_VERIFY_WITH_TIMEOUT(server.roundTripMs() > 0, 5000);

  // Same machine, same clock: the offset estimate settles close to zero. It can be
  // off by half a round trip, and the first pings are slow on a busy machine, so
  // give it a few more pings to find a quick exchange.
  QTRY_VERIFY(client.clockOffsetUs().has_value());
  QTRY_VERIFY2_WITH_TIMEOUT(
      std::llabs(*client.clockOffsetUs()) < 5000, qPrintable(QString::number(*client.clockOffsetUs())), 5000
  );
  // Capture-to-playout latency: the jitter buffer plus this loop's pacing.
  QTRY_VERIFY_WITH_TIMEOUT(client.latencyMs(StreamKind::System).has_value(), 3000);
  const double latency = *client.latencyMs(StreamKind::System);
  QVERIFY2(latency > 5 && latency < 150, qPrintable(QString::number(latency)));
  client.setOutputLatency(StreamKind::System, 20);
  QVERIFY(std::fabs(*client.latencyMs(StreamKind::System) - latency - 20) < 0.01);
  // The receiver reports it to the sender too.
  QTRY_VERIFY_WITH_TIMEOUT(server.remoteStats(StreamKind::System).contains("latencyMs"), 3000);
}

void AudioPeerTests::test_untrustedPeerIsRejected()
{
  const auto port = freePort();
  QVERIFY(port != 0);
  // The client expects some other server.
  AudioPeer server(serverConfig(port, m_certificates.first, TestCertificates::sha256(m_certificates.second)));
  AudioPeer client(clientConfig(port, m_certificates.second, QByteArray(32, '\x42')));
  QSignalSpy serverConnected(&server, &AudioPeer::connected);
  QSignalSpy clientConnected(&client, &AudioPeer::connected);
  QVERIFY(server.start());
  QVERIFY(client.start());
  QTest::qWait(1500);
  QCOMPARE(serverConnected.count(), 0);
  QCOMPARE(clientConnected.count(), 0);
}

void AudioPeerTests::test_stopSendingClosesStream()
{
  const auto port = freePort();
  QVERIFY(port != 0);
  AudioPeer server(serverConfig(port, m_certificates.first, TestCertificates::sha256(m_certificates.second)));
  AudioPeer client(clientConfig(port, m_certificates.second, TestCertificates::sha256(m_certificates.first)));
  QSignalSpy changed(&server, &AudioPeer::receiveStreamChanged);
  QVERIFY(server.start());
  QVERIFY(client.start());
  QTRY_VERIFY_WITH_TIMEOUT(server.isConnected() && client.isConnected(), 5000);
  QVERIFY(!server.receiveStream(StreamKind::Microphone));

  // Switching on while connected offers the stream straight away...
  client.setSending(StreamKind::Microphone, true);
  QTRY_VERIFY(server.receiveStream(StreamKind::Microphone));
  QVERIFY(changed.count() >= 1);
  // ...and switching off withdraws it.
  client.setSending(StreamKind::Microphone, false);
  QTRY_VERIFY(!server.receiveStream(StreamKind::Microphone));
  QVERIFY(!client.sendStream(StreamKind::Microphone));
}

void AudioPeerTests::test_clientReconnects()
{
  const auto port = freePort();
  QVERIFY(port != 0);
  auto server = std::make_unique<AudioPeer>(
      serverConfig(port, m_certificates.first, TestCertificates::sha256(m_certificates.second))
  );
  AudioPeer client(clientConfig(port, m_certificates.second, TestCertificates::sha256(m_certificates.first)));
  server->setSending(StreamKind::System, true);
  QVERIFY(server->start());
  QVERIFY(client.start());
  QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 5000);
  const auto first = client.receiveStream(StreamKind::System);
  QVERIFY(first);

  // The server goes away (Deskflow restarted, computer asleep) and comes back.
  server.reset();
  QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 5000);
  QTRY_VERIFY(!client.receiveStream(StreamKind::System));
  server = std::make_unique<AudioPeer>(
      serverConfig(port, m_certificates.first, TestCertificates::sha256(m_certificates.second))
  );
  server->setSending(StreamKind::System, true);
  QVERIFY(server->start());
  QTRY_VERIFY_WITH_TIMEOUT(client.isConnected() && client.receiveStream(StreamKind::System), 15000);
  // A fresh stream with fresh keys.
  QVERIFY(client.receiveStream(StreamKind::System) != first);
  exchange(*server, client, 30);
  QVERIFY(client.receiveStream(StreamKind::System)->stats().packets > 15);
}

QTEST_MAIN(AudioPeerTests)
