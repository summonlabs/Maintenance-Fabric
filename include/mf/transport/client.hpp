#pragma once

// Blocking request/response client with an explicit deadline. Request identity
// is monotonic per connection so a reordered or duplicated response is detected
// rather than accepted.

#include <cstdint>
#include <string>
#include <string_view>

#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/transport/frame.hpp"
#include "mf/transport/socket.hpp"

namespace mf {

class TcpClient {
 public:
  TcpClient() = default;
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;
  TcpClient(TcpClient&&) noexcept = default;
  TcpClient& operator=(TcpClient&&) noexcept = default;

  [[nodiscard]] static Result<TcpClient> connect(std::string_view host, std::uint16_t port,
                                                 Nanos timeout);
  [[nodiscard]] Result<Frame> call(const Frame& request, Nanos deadline);
  [[nodiscard]] Result<Frame> call_type(FrameType type, std::uint16_t op,
                                        const std::vector<std::byte>& body, Nanos deadline);
  [[nodiscard]] Status close();

  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] std::uint64_t next_request_id() noexcept { return next_id_++; }

 private:
  Socket socket_;
  std::uint64_t next_id_{1};
};

}  // namespace mf
