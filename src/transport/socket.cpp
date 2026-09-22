// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/transport/socket.hpp"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "mf/core/limits.hpp"

namespace mf {
namespace {

#ifdef _WIN32
constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(INVALID_SOCKET);
using NativeSocket = SOCKET;

NativeSocket native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }

Status last_error(std::string_view what) {
  const int code = WSAGetLastError();
  if (code == WSAETIMEDOUT) {
    return make_error(ErrorCode::Timeout, std::string(what) + ": timed out");
  }
  if (code == WSAEWOULDBLOCK) {
    return make_error(ErrorCode::Timeout, std::string(what) + ": would block");
  }
  return make_error(ErrorCode::IoError,
                    std::string(what) + ": winsock error " + std::to_string(code));
}
#else
constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(-1);
using NativeSocket = int;

NativeSocket native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }

Status last_error(std::string_view what) {
  const int code = errno;
  if (code == EAGAIN || code == EWOULDBLOCK) {
    return make_error(ErrorCode::Timeout, std::string(what) + ": timed out");
  }
  return make_error(ErrorCode::IoError,
                    std::string(what) + ": errno " + std::to_string(code));
}
#endif

}  // namespace

void Socket::ensure_initialized() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, []() {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != kInvalid; }

void Socket::close() {
  if (!valid()) {
    return;
  }
#ifdef _WIN32
  (void)closesocket(native(handle_));
#else
  (void)::close(native(handle_));
#endif
  handle_ = kInvalid;
}

Status Socket::shutdown_both() {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
#ifdef _WIN32
  if (::shutdown(native(handle_), SD_BOTH) != 0) {
    return ok_status();
  }
#else
  (void)::shutdown(native(handle_), SHUT_RDWR);
#endif
  return ok_status();
}

Status Socket::set_reuse_address() {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  int one = 1;
  if (::setsockopt(native(handle_), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&one), sizeof(one)) != 0) {
    return last_error("setsockopt(SO_REUSEADDR)");
  }
  return ok_status();
}

Status Socket::set_no_delay() {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  int one = 1;
  if (::setsockopt(native(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&one), sizeof(one)) != 0) {
    return last_error("setsockopt(TCP_NODELAY)");
  }
  return ok_status();
}

Status Socket::set_recv_timeout(Nanos timeout) {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  const Nanos clamped = timeout <= 0 ? 1 : timeout;
  const auto milliseconds = static_cast<long>(clamped / kNanosPerMillisecond);
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(milliseconds == 0 ? 1 : milliseconds);
  if (::setsockopt(native(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return last_error("setsockopt(SO_RCVTIMEO)");
  }
#else
  timeval value{};
  value.tv_sec = static_cast<time_t>(milliseconds / 1000);
  value.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
  if (::setsockopt(native(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return last_error("setsockopt(SO_RCVTIMEO)");
  }
#endif
  return ok_status();
}

Status Socket::set_send_timeout(Nanos timeout) {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  const Nanos clamped = timeout <= 0 ? 1 : timeout;
  const auto milliseconds = static_cast<long>(clamped / kNanosPerMillisecond);
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(milliseconds == 0 ? 1 : milliseconds);
  if (::setsockopt(native(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return last_error("setsockopt(SO_SNDTIMEO)");
  }
#else
  timeval value{};
  value.tv_sec = static_cast<time_t>(milliseconds / 1000);
  value.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
  if (::setsockopt(native(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return last_error("setsockopt(SO_SNDTIMEO)");
  }
#endif
  return ok_status();
}

Result<Socket> Socket::listen_tcp(std::string_view address, std::uint16_t port, int backlog) {
  ensure_initialized();
  NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == static_cast<NativeSocket>(kInvalid)) {
    return last_error("socket()").error();
  }
  Socket socket(static_cast<std::uintptr_t>(handle));
  const Status reuse = socket.set_reuse_address();
  if (!reuse.ok()) {
    return reuse.error();
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  const std::string host(address);
  if (host.empty() || host == "0.0.0.0" || host == "*") {
    endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, host.c_str(), &endpoint.sin_addr) != 1) {
    return invalid_argument("bind address is not a valid IPv4 address: " + host);
  }
  if (::bind(handle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    return last_error("bind()").error();
  }
  if (::listen(handle, backlog <= 0 ? 16 : backlog) != 0) {
    return last_error("listen()").error();
  }
  return socket;
}

Result<Socket> Socket::connect_tcp(std::string_view host, std::uint16_t port, Nanos timeout) {
  ensure_initialized();
  NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == static_cast<NativeSocket>(kInvalid)) {
    return last_error("socket()").error();
  }
  Socket socket(static_cast<std::uintptr_t>(handle));
  (void)socket.set_no_delay();
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  const std::string name(host);
  if (::inet_pton(AF_INET, name.c_str(), &endpoint.sin_addr) != 1) {
    return invalid_argument("host is not a valid IPv4 address: " + name);
  }
  if (::connect(handle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    return last_error("connect()").error();
  }
  if (timeout > 0) {
    (void)socket.set_recv_timeout(timeout);
    (void)socket.set_send_timeout(timeout);
  }
  return socket;
}

Result<Socket> Socket::accept() const {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  sockaddr_in peer{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(peer));
#else
  socklen_t length = sizeof(peer);
#endif
  const NativeSocket handle =
      ::accept(native(handle_), reinterpret_cast<sockaddr*>(&peer), &length);
  if (handle == static_cast<NativeSocket>(kInvalid)) {
    return last_error("accept()").error();
  }
  return Socket(static_cast<std::uintptr_t>(handle));
}

Status Socket::send_all(std::span<const std::byte> data) {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int written = ::send(native(handle_),
                               reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
    if (written <= 0) {
      return last_error("send()");
    }
    sent += static_cast<std::size_t>(written);
  }
  return ok_status();
}

Result<std::size_t> Socket::recv_some(std::span<std::byte> buffer) {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  if (buffer.empty()) {
    return invalid_argument("receive buffer is empty");
  }
  const int chunk = static_cast<int>(buffer.size() > 0x40000000u ? 0x40000000u : buffer.size());
  const int got = ::recv(native(handle_), reinterpret_cast<char*>(buffer.data()), chunk, 0);
  if (got < 0) {
    return last_error("recv()").error();
  }
  return static_cast<std::size_t>(got);
}

Status Socket::recv_exact(std::span<std::byte> buffer) {
  std::size_t received = 0;
  while (received < buffer.size()) {
    Result<std::size_t> got = recv_some(buffer.subspan(received));
    if (!got.ok()) {
      return got.error();
    }
    if (got.value() == 0) {
      return make_error(ErrorCode::IoError, "peer closed the connection");
    }
    received += got.value();
  }
  return ok_status();
}

std::uint16_t Socket::local_port() const {
  if (!valid()) {
    return 0;
  }
  sockaddr_in endpoint{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(endpoint));
#else
  socklen_t length = sizeof(endpoint);
#endif
  if (::getsockname(native(handle_), reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
    return 0;
  }
  return ntohs(endpoint.sin_port);
}

}  // namespace mf
