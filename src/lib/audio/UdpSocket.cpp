/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/UdpSocket.h"

#include <QNetworkInterface>

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
using NativeSocket = SOCKET;
static const NativeSocket kInvalid = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using NativeSocket = int;
static const NativeSocket kInvalid = -1;
#endif

namespace deskflow::audio {

namespace {

NativeSocket native(std::intptr_t socket)
{
  return NativeSocket(socket);
}

#if defined(_WIN32)
void ensureWinsock()
{
  static const bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}
#endif

sockaddr_in6 toSockaddr(const QHostAddress &address, quint16 port)
{
  sockaddr_in6 result{};
  result.sin6_family = AF_INET6;
  result.sin6_port = htons(port);
  // IPv4 addresses become IPv4-mapped IPv6 addresses (::ffff:a.b.c.d).
  const Q_IPV6ADDR raw = address.toIPv6Address();
  std::memcpy(&result.sin6_addr, raw.c, 16);
  const auto scope = address.scopeId();
  if (!scope.isEmpty()) {
    bool numeric = false;
    const auto index = scope.toUInt(&numeric);
    result.sin6_scope_id = numeric ? index : std::uint32_t(QNetworkInterface::interfaceIndexFromName(scope));
  }
  return result;
}

} // namespace

UdpSocket::UdpSocket() : m_socket(std::intptr_t(kInvalid))
{
#if defined(_WIN32)
  ensureWinsock();
#endif
}

UdpSocket::~UdpSocket()
{
  close();
}

bool UdpSocket::bind(quint16 port, QString *error)
{
  close();
  NativeSocket s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) {
    if (error) {
      *error = QStringLiteral("cannot create UDP socket");
    }
    return false;
  }
  int off = 0;
  setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof(off));
  int buffer = 512 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
#if defined(_WIN32)
  // Otherwise an ICMP "port unreachable" makes the next recvfrom() fail.
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif

  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(port);
  address.sin6_addr = in6addr_any;
  if (::bind(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
    if (error) {
      *error = QStringLiteral("cannot bind UDP port %1").arg(port);
    }
    return false;
  }
  sockaddr_in6 bound{};
  socklen_t length = sizeof(bound);
  getsockname(s, reinterpret_cast<sockaddr *>(&bound), &length);
  m_port = ntohs(bound.sin6_port);
  m_socket.store(std::intptr_t(s));
  return true;
}

quint16 UdpSocket::localPort() const
{
  return m_port;
}

bool UdpSocket::isOpen() const
{
  return native(m_socket.load()) != kInvalid;
}

bool UdpSocket::sendTo(const std::uint8_t *data, int size, const QHostAddress &address, quint16 port)
{
  if (!isOpen()) {
    return false;
  }
  const auto target = toSockaddr(address, port);
  const auto sent = ::sendto(
      native(m_socket.load()), reinterpret_cast<const char *>(data), size, 0, reinterpret_cast<const sockaddr *>(&target),
      sizeof(target)
  );
  return sent == size;
}

int UdpSocket::receive(std::uint8_t *buffer, int capacity, QHostAddress *from, quint16 *fromPort, int timeoutMs)
{
  if (!isOpen()) {
    return -1;
  }
#if defined(_WIN32)
  WSAPOLLFD poller{};
  poller.fd = native(m_socket.load());
  poller.events = POLLRDNORM;
  const int ready = WSAPoll(&poller, 1, timeoutMs);
#else
  pollfd poller{};
  poller.fd = native(m_socket.load());
  poller.events = POLLIN;
  const int ready = ::poll(&poller, 1, timeoutMs);
#endif
  if (ready == 0) {
    return 0;
  }
  if (ready < 0) {
#if !defined(_WIN32)
    if (errno == EINTR) {
      return 0;
    }
#endif
    return -1;
  }
  sockaddr_storage source{};
  socklen_t length = sizeof(source);
  const auto received = ::recvfrom(
      native(m_socket.load()), reinterpret_cast<char *>(buffer), capacity, 0, reinterpret_cast<sockaddr *>(&source), &length
  );
  if (received < 0) {
#if defined(_WIN32)
    const int code = WSAGetLastError();
    return (code == WSAEMSGSIZE || code == WSAECONNRESET) ? 0 : -1;
#else
    return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
#endif
  }
  if (from) {
    *from = QHostAddress(reinterpret_cast<const sockaddr *>(&source));
  }
  if (fromPort) {
    *fromPort = source.ss_family == AF_INET6 ? ntohs(reinterpret_cast<const sockaddr_in6 *>(&source)->sin6_port)
                                             : ntohs(reinterpret_cast<const sockaddr_in *>(&source)->sin_port);
  }
  return int(received);
}

void UdpSocket::close()
{
  const auto socket = native(m_socket.exchange(std::intptr_t(kInvalid)));
  if (socket == kInvalid) {
    return;
  }
#if defined(_WIN32)
  closesocket(socket);
#else
  ::shutdown(socket, SHUT_RDWR);
  ::close(socket);
#endif
  m_port = 0;
}

} // namespace deskflow::audio
