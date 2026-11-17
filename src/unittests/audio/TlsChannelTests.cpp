/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TlsChannelTests.h"

#include "audio/TlsChannel.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

using deskflow::audio::TlsChannel;

namespace {

struct Pair
{
  QTcpServer server;
  std::unique_ptr<TlsChannel> serverSide;
  std::unique_ptr<TlsChannel> clientSide;

  bool connect()
  {
    if (!server.listen(QHostAddress::LocalHost, 0)) {
      return false;
    }
    auto *socket = new QTcpSocket;
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    if (!QTest::qWaitFor([&] { return server.hasPendingConnections() && socket->state() == QAbstractSocket::ConnectedState; })) {
      delete socket;
      return false;
    }
    serverSide = std::make_unique<TlsChannel>(server.nextPendingConnection(), TlsChannel::Mode::Server);
    clientSide = std::make_unique<TlsChannel>(socket, TlsChannel::Mode::Client);
    return true;
  }
};

} // namespace

void TlsChannelTests::initTestCase()
{
  QVERIFY(m_certificates.create());
}

void TlsChannelTests::test_handshakeAndMessages()
{
  Pair pair;
  QVERIFY(pair.connect());
  QSignalSpy serverReady(pair.serverSide.get(), &TlsChannel::ready);
  QSignalSpy clientReady(pair.clientSide.get(), &TlsChannel::ready);
  QSignalSpy serverReceived(pair.serverSide.get(), &TlsChannel::messageReceived);
  QSignalSpy clientReceived(pair.clientSide.get(), &TlsChannel::messageReceived);

  QString error;
  QVERIFY2(pair.serverSide->start(m_certificates.first, &error), qPrintable(error));
  QVERIFY2(pair.clientSide->start(m_certificates.second, &error), qPrintable(error));
  // Sent before the handshake finished: held until it has.
  pair.clientSide->send(QJsonObject{{"type", "early"}});

  QTRY_COMPARE(serverReady.count(), 1);
  QTRY_COMPARE(clientReady.count(), 1);
  QVERIFY(pair.serverSide->isReady());

  // Each side sees the other's certificate.
  QCOMPARE(pair.serverSide->peerSha256(), TestCertificates::sha256(m_certificates.second));
  QCOMPARE(pair.clientSide->peerSha256(), TestCertificates::sha256(m_certificates.first));

  QTRY_COMPARE(serverReceived.count(), 1);
  QCOMPARE(serverReceived.at(0).at(0).toJsonObject().value("type").toString(), QStringLiteral("early"));

  // Large messages are framed correctly across many TLS records.
  const QString big(150 * 1024, QLatin1Char('x'));
  pair.serverSide->send(QJsonObject{{"type", "big"}, {"data", big}});
  for (int i = 0; i < 20; ++i) {
    pair.serverSide->send(QJsonObject{{"type", "seq"}, {"n", i}});
  }
  QTRY_COMPARE(clientReceived.count(), 21);
  QCOMPARE(clientReceived.at(0).at(0).toJsonObject().value("data").toString(), big);
  for (int i = 0; i < 20; ++i) {
    QCOMPARE(clientReceived.at(i + 1).at(0).toJsonObject().value("n").toInt(), i);
  }
}

void TlsChannelTests::test_closeIsSeenByPeer()
{
  Pair pair;
  QVERIFY(pair.connect());
  QSignalSpy serverReady(pair.serverSide.get(), &TlsChannel::ready);
  QSignalSpy serverClosed(pair.serverSide.get(), &TlsChannel::closed);
  QVERIFY(pair.serverSide->start(m_certificates.first));
  QVERIFY(pair.clientSide->start(m_certificates.second));
  QTRY_COMPARE(serverReady.count(), 1);

  pair.clientSide->close(QStringLiteral("done"));
  QTRY_COMPARE(serverClosed.count(), 1);
}

void TlsChannelTests::test_garbageIsRejected()
{
  // A peer that does not speak TLS fails the handshake instead of hanging.
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  QTcpSocket raw;
  raw.connectToHost(QHostAddress::LocalHost, server.serverPort());
  QTRY_VERIFY(server.hasPendingConnections());
  TlsChannel channel(server.nextPendingConnection(), TlsChannel::Mode::Server);
  QSignalSpy closed(&channel, &TlsChannel::closed);
  QSignalSpy ready(&channel, &TlsChannel::ready);
  QVERIFY(channel.start(m_certificates.first));
  QTRY_COMPARE(raw.state(), QAbstractSocket::ConnectedState);
  raw.write(QByteArray(600, 'G'));
  QTRY_COMPARE(closed.count(), 1);
  QCOMPARE(ready.count(), 0);
}

QTEST_MAIN(TlsChannelTests)
