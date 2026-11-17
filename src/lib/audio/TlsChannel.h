/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>

class QTcpSocket;
struct ssl_st;
struct ssl_ctx_st;
struct bio_st;

namespace deskflow::audio {

/**
 * Length-prefixed JSON messages over TLS on a QTcpSocket, using OpenSSL
 * memory BIOs so everything runs on the owning thread's event loop.
 *
 * Both sides present the Deskflow certificate. Certificate chains are not
 * checked here; the owner must compare peerSha256() with its trusted
 * fingerprints once ready() fires, the same way Deskflow itself does.
 */
class TlsChannel : public QObject
{
  Q_OBJECT
public:
  enum class Mode
  {
    Client,
    Server
  };

  //! Takes ownership of a connected socket.
  TlsChannel(QTcpSocket *socket, Mode mode, QObject *parent = nullptr);
  ~TlsChannel() override;

  //! Load the PEM file (certificate and private key) and begin the handshake.
  bool start(const QString &pemPath, QString *error = nullptr);

  bool isReady() const
  {
    return m_ready;
  }

  void send(const QJsonObject &message);
  void close(const QString &reason);

  //! SHA-256 of the peer's certificate, empty before the handshake completes.
  QByteArray peerSha256() const;

  QTcpSocket *socket() const
  {
    return m_socket;
  }

Q_SIGNALS:
  void ready();
  void messageReceived(const QJsonObject &message);
  void closed(const QString &reason);

private:
  void onReadyRead();
  void continueHandshake();
  void readPlaintext();
  void writeFrame(const QByteArray &frame);
  void flushOutgoing();
  void fail(const QString &reason);

  QTcpSocket *m_socket;
  Mode m_mode;
  ssl_ctx_st *m_context = nullptr;
  ssl_st *m_ssl = nullptr;
  bio_st *m_in = nullptr;  // network -> OpenSSL, owned by m_ssl
  bio_st *m_out = nullptr; // OpenSSL -> network, owned by m_ssl
  bool m_ready = false;
  bool m_closed = false;
  QByteArray m_inbox;
  QList<QByteArray> m_queued;
  QByteArray m_peerSha256;
};

} // namespace deskflow::audio
