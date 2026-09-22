#pragma once

// Integration boundary to Drain Fabric. Maintenance Fabric never removes
// service itself: it asks Drain Fabric for a drain lease covering the removal
// set, holds that lease while the resource is out of service, and releases it
// only after restoration has been verified. A drain failure is a block, never a
// silent continue.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/identity.hpp"

namespace mf {

enum class DrainState : std::uint8_t {
  Unknown = 0,
  Draining = 1,
  Drained = 2,
  Failed = 3,
  Released = 4,
  NotFound = 5,
};

[[nodiscard]] const char* to_string(DrainState state) noexcept;
[[nodiscard]] std::optional<DrainState> parse_drain_state(std::string_view text) noexcept;

struct DrainRequest {
  JobId job;
  GenerationId generation;
  AttemptId attempt;
  std::vector<TargetId> targets;
  Nanos lease_duration{limits::kDefaultDrainLeaseNanos};
  std::string reason;
  ControllerIncarnation requester;
};

struct DrainLease {
  DrainLeaseId id;
  std::vector<TargetId> targets;
  Nanos granted_at{0};
  Nanos expires_at{0};
  DrainState state{DrainState::Unknown};
  AttemptId attempt;
  ControllerIncarnation owner;
  std::uint64_t epoch{0};
};

struct DrainResponse {
  Status status;
  DrainLease lease;

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

/// Abstract client of an external drain service. Implementations must be safe
/// to call without holding any runtime lock (the controller releases its lock
/// before every call).
class DrainPort {
 public:
  DrainPort() = default;
  virtual ~DrainPort();
  DrainPort(const DrainPort&) = delete;
  DrainPort& operator=(const DrainPort&) = delete;

  [[nodiscard]] virtual DrainResponse request_drain(const DrainRequest& request) = 0;
  [[nodiscard]] virtual DrainResponse refresh_drain(DrainLeaseId lease, Nanos extend,
                                                    const ControllerIncarnation& requester) = 0;
  [[nodiscard]] virtual DrainResponse query_drain(DrainLeaseId lease,
                                                  const ControllerIncarnation& requester) = 0;
  [[nodiscard]] virtual Status release_drain(DrainLeaseId lease,
                                             const ControllerIncarnation& requester) = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

using DrainPortPtr = std::shared_ptr<DrainPort>;

/// Validates a request against the bounded drain surface.
[[nodiscard]] Status validate_drain_request(const DrainRequest& request);

}  // namespace mf
