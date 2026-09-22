#pragma once

// Client of a real, out-of-process drain service over the framed TCP transport.
// Connection failures are reported as drain failures; the controller treats them
// as a block, never as an implicit success.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <vector>

#include "mf/domain/drain.hpp"
#include "mf/transport/client.hpp"
#include "mf/transport/protocol.hpp"

namespace mf {

struct RemoteDrainOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  Nanos deadline{limits::kDefaultRpcDeadlineNanos};
  std::uint32_t connect_attempts{2};
};

class RemoteDrainPort final : public DrainPort {
 public:
  explicit RemoteDrainPort(RemoteDrainOptions options);
  ~RemoteDrainPort() override;

  [[nodiscard]] DrainResponse request_drain(const DrainRequest& request) override;
  [[nodiscard]] DrainResponse refresh_drain(DrainLeaseId lease, Nanos extend,
                                            const ControllerIncarnation& requester) override;
  [[nodiscard]] DrainResponse query_drain(DrainLeaseId lease,
                                          const ControllerIncarnation& requester) override;
  [[nodiscard]] Status release_drain(DrainLeaseId lease,
                                     const ControllerIncarnation& requester) override;
  [[nodiscard]] std::string_view name() const noexcept override { return "remote-drain"; }

  [[nodiscard]] bool connected() const;
  void disconnect();
  [[nodiscard]] std::uint64_t calls() const;
  [[nodiscard]] std::uint64_t failures() const;

 private:
  [[nodiscard]] Result<std::vector<std::byte>> call(OpCode op, const std::vector<std::byte>& body);
  [[nodiscard]] bool ensure_connected_locked();

  RemoteDrainOptions options_;
  mutable std::mutex mu_;
  std::optional<TcpClient> client_;
  std::uint64_t calls_{0};
  std::uint64_t failures_{0};
};

}  // namespace mf
