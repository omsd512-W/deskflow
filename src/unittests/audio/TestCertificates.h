/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/SecureUtils.h"

#include <QFile>
#include <QTemporaryDir>

#include <openssl/pem.h>

//! Two self-signed certificates in a temporary directory, as two computers would have.
struct TestCertificates
{
  QTemporaryDir dir;
  QString first;
  QString second;

  bool create()
  {
    if (!dir.isValid()) {
      return false;
    }
    first = dir.filePath(QStringLiteral("first.pem"));
    second = dir.filePath(QStringLiteral("second.pem"));
    deskflow::generatePemSelfSignedCert(first);
    deskflow::generatePemSelfSignedCert(second);
    return !sha256(first).isEmpty() && !sha256(second).isEmpty();
  }

  static QByteArray sha256(const QString &path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      return {};
    }
    const auto pem = file.readAll();
    BIO *bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) {
      return {};
    }
    const auto digest = deskflow::sslCertFingerprint(cert, QCryptographicHash::Sha256).data;
    X509_free(cert);
    return digest;
  }
};
