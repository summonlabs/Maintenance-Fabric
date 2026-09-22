#pragma once

// Deterministic arbitration of conflicting maintenance jobs. The order is a
// total order on (policy tier rank, request creation time, job id); the same
// inputs always produce the same decisions, and every decision records the
// ordering key and the reason an alternative was rejected.

#include <cstdint>
#include <string>
#include <vector>

#include "mf/domain/authority.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/engine/authority.hpp"
#include "mf/engine/readiness.hpp"

namespace mf {

struct ArbitrationCandidate {
  JobId job;
  std::uint32_t tier{0};
  std::uint32_t tier_rank{0};
  Nanos created_at{0};
  std::string ordering_key;
};

/// Total order used by the arbitrator: tier rank ascending (0 is most urgent),
/// then creation time ascending, then job id ascending.
[[nodiscard]] std::vector<ArbitrationCandidate> order_candidates(
    const std::vector<const MaintenanceJob*>& jobs, const Policy& policy);

enum class ArbitrationOutcome : std::uint8_t { Granted = 0, Denied = 1, Deferred = 2 };

[[nodiscard]] const char* to_string(ArbitrationOutcome outcome) noexcept;

struct ArbitrationDecision {
  JobId job;
  ArbitrationOutcome outcome{ArbitrationOutcome::Deferred};
  std::size_t position{0};
  std::uint32_t tier_rank{0};
  Nanos created_at{0};
  std::string ordering_key;
  BlockReason reason{BlockReason::None};
  std::string detail;
  AuthorityId authority;
  std::uint64_t impact_digest{0};
};

struct ArbitrationReport {
  std::vector<ArbitrationDecision> decisions;
  Nanos at{0};
  ControllerIncarnation by;
  Revision policy_revision;
  Revision topology_revision;
  std::uint64_t policy_digest{0};
  std::uint64_t topology_digest{0};
  std::size_t granted{0};
  std::size_t denied{0};
  std::size_t deferred{0};
  std::uint64_t digest{0};

  [[nodiscard]] std::string to_text() const;
};

struct ArbitrationContext {
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  const ReadinessView* readiness{nullptr};
  AuthorityLedger* ledger{nullptr};
  const JobTable* jobs{nullptr};
  std::vector<JobId> candidates;
  ControllerIncarnation current;
  Nanos now{0};
  /// Optional out-parameter receiving the projection of the first grant.
  CapacityImpact* impact_out{nullptr};
};

[[nodiscard]] ArbitrationReport arbitrate(const ArbitrationContext& context);

}  // namespace mf
