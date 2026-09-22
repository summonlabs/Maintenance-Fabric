#pragma once

// Minimal, bounded TCP socket wrapper. All reads and writes are deadline aware
// and every buffer length is supplied by the caller, so no receive path ever
// allocates from an untrusted length.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/version.hpp"

namespace mf {

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  /// One-time process-wide socket subsystem initialisation. Safe to call from
  /// multiple threads.
  static void ensure_initialized();

  [[nodiscard]] static Result<Socket> listen_tcp(std::string_view address, std::uint16_t port,
                                                 int backlog);
  [[nodiscard]] static Result<Socket> connect_tcp(std::string_view host, std::uint16_t port,
                                                  Nanos timeout);
  [[nodiscard]] Result<Socket> accept() const;

  [[nodiscard]] Status send_all(std::span<const std::byte> data);
  [[nodiscard]] Result<std::size_t> recv_some(std::span<std::byte> buffer);
  [[nodiscard]] Status recv_exact(std::span<std::byte> buffer);

  [[nodiscard]] Status set_recv_timeout(Nanos timeout);
  [[nodiscard]] Status set_send_timeout(Nanos timeout);
  [[nodiscard]] Status set_no_delay();
  [[nodiscard]] Status set_reuse_address();
  [[nodiscard]] Status shutdown_both();
  void close();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t local_port() const;

 private:
  explicit Socket(std::uintptr_t handle) noexcept : handle_(handle) {}

  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
};

}  // namespace mf
