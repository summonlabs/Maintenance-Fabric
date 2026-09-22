#pragma once

// Authority acquisition. The joint safety check happens here, under the
// controller's single writer lock: the plan is projected against the union of
// every live reservation, so two jobs cannot independently consume the same
// correlated tolerance budget.

#include <cstdint>
#include <vector>

#include "mf/domain/authority.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/engine/impact.hpp"
#include "mf/engine/readiness.hpp"

namespace mf {

struct AuthorityAcquireContext {
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  const ReadinessView* readiness{nullptr};
  AuthorityLedger* ledger{nullptr};
  JobId job;
  GenerationId generation;
  AttemptId attempt;
  std::vector<TargetId> removal_set;
  ControllerIncarnation current;
  Nanos now{0};
  /// Zero selects policy.authority_lease.
  Nanos lease_duration{0};
  /// Receives the projection that justified the grant.
  CapacityImpact* impact_out{nullptr};
};

/// Returns the granted authority id, or the policy/limit error that refused it.
[[nodiscard]] Result<AuthorityId> acquire_authority(const AuthorityAcquireContext& context);

}  // namespace mf
