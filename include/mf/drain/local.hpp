#pragma once

// In-process reference implementation of the external Drain Fabric contract.
//
// This is a simulator of a foreign system, not part of the Maintenance Fabric
// runtime: it exists so tests, examples and benchmarks can exercise the drain
// boundary deterministically, including injected failures. Production
// deployments use RemoteDrainPort against a real drain service.

#include <cstdint>
#include <map>
#include <mutex>

#include "mf/domain/drain.hpp"

namespace mf {

struct LocalDrainOptions {
  /// State a granted lease settles into. Draining models an asynchronous
  /// drain service; Drained models one that completes synchronously.
  DrainState granted_state{DrainState::Drained};
  /// Number of upcoming request_drain calls that fail before succeeding.
  std::uint32_t fail_next_requests{0};
  /// Refuse refresh calls whose requester is not the lease owner.
  bool fence_refresh_by_owner{true};
  /// Drop every lease when the next query arrives (simulates a drain service
  /// that lost its state).
  bool drop_leases_on_next_query{false};
};

class LocalDrainPort final : public DrainPort {
 public:
  explicit LocalDrainPort(LocalDrainOptions options = {});
  ~LocalDrainPort() override;

  [[nodiscard]] DrainResponse request_drain(const DrainRequest& request) override;
  [[nodiscard]] DrainResponse refresh_drain(DrainLeaseId lease, Nanos extend,
                                            const ControllerIncarnation& requester) override;
  [[nodiscard]] DrainResponse query_drain(DrainLeaseId lease,
                                          const ControllerIncarnation& requester) override;
  [[nodiscard]] Status release_drain(DrainLeaseId lease,
                                     const ControllerIncarnation& requester) override;
  [[nodiscard]] std::string_view name() const noexcept override { return "local-drain-sim"; }

  // --- test controls --------------------------------------------------------
  void fail_next(std::uint32_t count);
  void set_granted_state(DrainState state);
  void drop_leases_on_next_query();
  [[nodiscard]] std::size_t active_leases() const;
  [[nodiscard]] std::uint64_t requests() const;
  [[nodiscard]] std::uint64_t releases() const;
  [[nodiscard]] std::uint64_t refreshes() const;
  [[nodiscard]] bool holds(DrainLeaseId lease) const;

 private:
  mutable std::mutex mu_;
  LocalDrainOptions options_;
  std::map<DrainLeaseId, DrainLease> leases_;
  DrainLeaseId next_id_{DrainLeaseId::from_u64(1)};
  std::uint64_t epoch_{0};
  std::uint64_t requests_{0};
  std::uint64_t releases_{0};
  std::uint64_t refreshes_{0};
};

}  // namespace mf
