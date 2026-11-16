/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/TlsChannel.h"

#include "net/SecureUtils.h"

#include <QJsonDocument>
#include <QTcpSocket>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace deskflow::audio {

namespace {

constexpr int kMaxMessageBytes = 256 * 1024;

QString lastSslError()
{
  const auto code = ERR_get_error();
  if (code == 0) {
    return QStringLiteral("unknown TLS error");
  }
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return QString::fromLatin1(text);
}

int acceptAnyCertificate(int, X509_STORE_CTX *)
{
  // Self-signed certificates are the norm; trust is decided by fingerprint.
  return 1;
}

} // namespace

TlsChannel::TlsChannel(QTcpSocket *socket, Mode mode, QObject *parent)
    : QObject(parent),
      m_socket(socket),
      m_mode(mode)
{
  m_socket->setParent(this);
  connect(m_socket, &QTcpSocket::readyRead, this, &TlsChannel::onReadyRead);
  connect(m_socket, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("connection closed")); });
  connect(m_socket, &QTcpSocket::errorOccurred, this, [this] { fail(m_socket->errorString()); });
}

TlsChannel::~TlsChannel()
{
  if (m_ssl) {
    SSL_free(m_ssl); // also frees both BIOs
  }
  if (m_context) {
    SSL_CTX_free(m_context);
  }
}

bool TlsChannel::start(const QString &pemPath, QString *error)
{
  const auto setError = [error](const QString &text) {
    if (error) {
      *error = text;
    }
    return false;
  };

  m_context = SSL_CTX_new(TLS_method());
  if (!m_context) {
    return setError(lastSslError());
  }
  SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
  const auto path = pemPath.toLocal8Bit();
  if (SSL_CTX_use_certificate_file(m_context, path.constData(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(m_context, path.constData(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(m_context) != 1) {
    return setError(QStringLiteral("cannot load certificate %1: %2").arg(pemPath, lastSslError()));
  }
  int verify = SSL_VERIFY_PEER;
  if (m_mode == Mode::Server) {
    verify |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  SSL_CTX_set_verify(m_context, verify, acceptAnyCertificate);

  m_ssl = SSL_new(m_context);
  m_in = BIO_new(BIO_s_mem());
  m_out = BIO_new(BIO_s_mem());
  SSL_set_bio(m_ssl, m_in, m_out);
  if (m_mode == Mode::Server) {
    SSL_set_accept_state(m_ssl);
  } else {
    SSL_set_connect_state(m_ssl);
  }
  continueHandshake();
  return true;
}

void TlsChannel::send(const QJsonObject &message)
{
  const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
  QByteArray frame(4, '\0');
  const auto size = quint32(body.size());
  frame[0] = char(size >> 24);
  frame[1] = char(size >> 16);
  frame[2] = char(size >> 8);
  frame[3] = char(size);
  frame.append(body);
  if (!m_ready) {
    m_queued.append(frame);
    return;
  }
  writeFrame(frame);
}

void TlsChannel::close(const QString &reason)
{
  if (m_closed) {
    return;
  }
  if (m_ssl && m_ready) {
    SSL_shutdown(m_ssl);
    flushOutgoing();
  }
  m_closed = true;
  m_ready = false;
  // Unlike abort(), this lets the close_notify and queued data go out first.
  m_socket->disconnectFromHost();
  Q_EMIT closed(reason);
}

QByteArray TlsChannel::peerSha256() const
{
  return m_peerSha256;
}

void TlsChannel::onReadyRead()
{
  if (m_closed || !m_ssl) {
    return;
  }
  const auto data = m_socket->readAll();
  if (!data.isEmpty()) {
    BIO_write(m_in, data.constData(), int(data.size()));
  }
  if (!m_ready) {
    continueHandshake();
  }
  if (m_ready) {
    readPlaintext();
  }
  flushOutgoing();
}

void TlsChannel::continueHandshake()
{
  const int result = SSL_do_handshake(m_ssl);
  flushOutgoing();
  if (result == 1) {
    X509 *certificate = SSL_get_peer_certificate(m_ssl);
    if (!certificate) {
      fail(QStringLiteral("peer sent no certificate"));
      return;
    }
    m_peerSha256 = deskflow::sslCertFingerprint(certificate, QCryptographicHash::Sha256).data;
    X509_free(certificate);
    m_ready = true;
    for (const auto &frame : std::as_const(m_queued)) {
      writeFrame(frame);
    }
    m_queued.clear();
    Q_EMIT ready();
    return;
  }
  const int code = SSL_get_error(m_ssl, result);
  if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
    fail(QStringLiteral("TLS handshake failed: %1").arg(lastSslError()));
  }
}

void TlsChannel::readPlaintext()
{
  char buffer[16384];
  while (!m_closed) {
    const int read = SSL_read(m_ssl, buffer, sizeof(buffer));
    if (read > 0) {
      m_inbox.append(buffer, read);
      continue;
    }
    const int code = SSL_get_error(m_ssl, read);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
      break;
    }
    if (code == SSL_ERROR_ZERO_RETURN) {
      fail(QStringLiteral("peer closed the session"));
    } else {
      fail(QStringLiteral("TLS read failed: %1").arg(lastSslError()));
    }
    return;
  }

  while (!m_closed && m_inbox.size() >= 4) {
    const auto *head = reinterpret_cast<const unsigned char *>(m_inbox.constData());
    const quint32 size = (quint32(head[0]) << 24) | (quint32(head[1]) << 16) | (quint32(head[2]) << 8) | head[3];
    if (size > quint32(kMaxMessageBytes)) {
      fail(QStringLiteral("control message too large"));
      return;
    }
    if (m_inbox.size() < qsizetype(4 + size)) {
      break;
    }
    const auto document = QJsonDocument::fromJson(m_inbox.mid(4, qsizetype(size)));
    m_inbox.remove(0, qsizetype(4 + size));
    if (document.isObject()) {
      Q_EMIT messageReceived(document.object());
    }
  }
}

void TlsChannel::writeFrame(const QByteArray &frame)
{
  if (m_closed) {
    return;
  }
  // Memory BIOs grow as needed, so a write never stalls.
  if (SSL_write(m_ssl, frame.constData(), int(frame.size())) <= 0) {
    fail(QStringLiteral("TLS write failed: %1").arg(lastSslError()));
    return;
  }
  flushOutgoing();
}

void TlsChannel::flushOutgoing()
{
  char buffer[16384];
  while (BIO_ctrl_pending(m_out) > 0) {
    const int read = BIO_read(m_out, buffer, sizeof(buffer));
    if (read <= 0) {
      break;
    }
    m_socket->write(buffer, read);
  }
}

void TlsChannel::fail(const QString &reason)
{
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_ready = false;
  if (m_socket->state() != QAbstractSocket::UnconnectedState) {
    m_socket->abort();
  }
  Q_EMIT closed(reason);
}

} // namespace deskflow::audio
