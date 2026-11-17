/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QHostAddress>
#include <QString>

#include <atomic>
#include <cstdint>

namespace deskflow::audio {

/**
 * A dual-stack UDP socket on plain OS sockets. sendTo() may be called from
 * any thread (the capture threads); receive() from one network thread.
 * Qt's QUdpSocket is avoided because it is tied to one thread's event loop.
 */
class UdpSocket
{
public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  //! Bind to all interfaces, IPv6 and IPv4. Port 0 picks a free port.
  bool bind(quint16 port, QString *error = nullptr);
  quint16 localPort() const;
  bool isOpen() const;

  bool sendTo(const std::uint8_t *data, int size, const QHostAddress &address, quint16 port);

  //! Wait up to timeoutMs for one datagram.
  //! @return its size, 0 on timeout, or -1 on error
  int receive(std::uint8_t *buffer, int capacity, QHostAddress *from, quint16 *fromPort, int timeoutMs);

  void close();

private:
  std::atomic<std::intptr_t> m_socket;
  std::atomic<quint16> m_port{0};
};

} // namespace deskflow::audio
