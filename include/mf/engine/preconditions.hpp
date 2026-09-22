#pragma once

// Authoritative precondition evaluation. A job may only start when every
// required precondition is Satisfied, under a recorded policy revision, a
// recorded topology revision and a recorded evidence digest. Pending differs
// from Violated: pending prerequisites may still be acquired (authority, drain),
// violated ones must be repaired before the job can progress.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mf/domain/authority.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/engine/impact.hpp"
#include "mf/engine/readiness.hpp"

namespace mf {

enum class PreconditionKind : std::uint16_t {
  PolicyBound = 0,
  RequestShape = 1,
  TargetsKnown = 2,
  TargetsMaintainable = 3,
  DependencySatisfied = 4,
  EvidenceFresh = 5,
  Redundancy = 6,
  FailureDomain = 7,
  CapacityHeadroom = 8,
  ContractAvailability = 9,
  ConflictingMaintenance = 10,
  WindowAdmissible = 11,
  ControlPlaneQuorum = 12,
  AuthorityReserved = 13,
  DrainAcquired = 14,
};

inline constexpr std::size_t kPreconditionKindCount = 15;

[[nodiscard]] const char* to_string(PreconditionKind kind) noexcept;
[[nodiscard]] std::optional<PreconditionKind> parse_precondition_kind(std::string_view text) noexcept;

enum class PreconditionStatus : std::uint8_t { Satisfied = 0, Violated = 1, Pending = 2 };

[[nodiscard]] const char* to_string(PreconditionStatus status) noexcept;

struct PreconditionResult {
  PreconditionKind kind{PreconditionKind::PolicyBound};
  PreconditionStatus status{PreconditionStatus::Satisfied};
  BlockReason reason{BlockReason::None};
  std::string code;
  std::string detail;
};

struct PreconditionSet {
  std::vector<PreconditionResult> results;
  std::size_t satisfied{0};
  std::size_t violated{0};
  std::size_t pending{0};
  bool all_satisfied{false};
  Nanos evaluated_at{0};
  ControllerIncarnation evaluated_by;
  Revision policy_revision;
  Revision topology_revision;
  std::uint64_t policy_digest{0};
  std::uint64_t topology_digest{0};
  std::uint64_t readiness_digest{0};
  std::uint64_t impact_digest{0};
  std::uint64_t digest{0};

  [[nodiscard]] std::optional<BlockReason> first_violation() const;
  [[nodiscard]] const PreconditionResult* find(PreconditionKind kind) const;
  [[nodiscard]] std::string to_text() const;
};

struct PreconditionContext {
  const MaintenanceJob* job{nullptr};
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  const EvidenceStore* evidence{nullptr};
  const AuthorityLedger* ledger{nullptr};
  const JobTable* jobs{nullptr};
  const ReadinessView* readiness{nullptr};
  ControllerIncarnation current;
  Nanos now{0};
  /// Phase B: authority and drain are expected to be held already.
  bool check_authority{false};
  /// Phase B: a drain lease must exist and be confirmed by fresh evidence.
  bool check_drain{false};
  /// Reservations of other jobs are already accounted for in this list; when
  /// null the concurrency implications of other jobs are still checked.
  CapacityImpact* impact_out{nullptr};
  const CapacityImpact* impact_in{nullptr};
};

[[nodiscard]] PreconditionSet evaluate_preconditions(const PreconditionContext& context);

/// Reasons that must never be auto-cleared by retrying; the caller surfaces them
/// to the operator instead.
[[nodiscard]] bool requires_operator_action(BlockReason reason) noexcept;

}  // namespace mf
