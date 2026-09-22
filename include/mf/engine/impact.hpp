#pragma once

// Capacity impact projection: exactly how much serving capacity a planned
// removal set would consume, evaluated against generic redundancy, correlated
// failure domain, capacity reserve and availability contract policy.
//
// This function is the single source of truth for "is this removal safe". It is
// called both to admit a new authorization and to re-check the union of every
// live authorization, which is what stops two individually admissible jobs from
// jointly removing unsafe correlated capacity.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/engine/readiness.hpp"

namespace mf {

struct TargetImpact {
  TargetId target;
  bool exists{false};
  bool maintainable{false};
  bool covered_by_baseline{false};
  bool planned{false};
  bool evidence_fresh{false};
  Health health{Health::Unknown};
  std::string detail;
};

struct PoolImpact {
  PoolId pool;
  std::string name;
  std::uint32_t total_units{0};
  std::uint32_t serving_units{0};
  std::uint32_t unknown_units{0};
  std::uint32_t removed_serving_units{0};
  std::uint32_t remaining_units{0};
  std::uint32_t required_units{0};
  std::uint32_t reserve_units{0};
  bool satisfied{true};
  std::string rule;
  std::string reason;
};

struct DomainImpact {
  DomainId domain;
  std::string name;
  bool limited{false};
  std::uint32_t out_before{0};
  std::uint32_t out_after{0};
  std::uint32_t max_out{0};
  bool satisfied{true};
  std::string rule;
  std::string reason;
};

struct ContractImpact {
  ContractId contract;
  std::string name;
  std::uint32_t available_before{0};
  std::uint32_t available_after{0};
  std::uint32_t min_available{0};
  bool satisfied{true};
  std::string reason;
};

struct CapacityImpact {
  std::vector<TargetImpact> targets;
  std::vector<PoolImpact> pools;
  std::vector<DomainImpact> domains;
  std::vector<ContractImpact> contracts;
  std::uint32_t planned_count{0};
  std::uint32_t baseline_count{0};
  bool satisfied{true};
  std::uint64_t digest{0};

  [[nodiscard]] std::optional<BlockReason> block_reason() const;
  [[nodiscard]] std::string first_violation() const;
  [[nodiscard]] std::string summary() const;
};

struct ImpactInput {
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  const ReadinessView* readiness{nullptr};
  /// Targets already out of service because another job holds authority.
  std::vector<TargetId> baseline_removed;
  /// Targets this job would take out of service.
  std::vector<TargetId> planned_removed;
  /// When false, unknown (unfreshened) pool members are reported but do not by
  /// themselves fail the redundancy rule. Used for preview/explain only.
  bool require_full_evidence{true};
};

[[nodiscard]] CapacityImpact compute_impact(const ImpactInput& input);

}  // namespace mf
