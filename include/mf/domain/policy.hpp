#pragma once

// Policy is data, not code. Redundancy tolerance, correlated-failure limits,
// capacity reserve, availability contracts, maintenance windows, priority tiers
// and evidence freshness bounds are all supplied by the operator. Nothing in
// the runtime hardcodes a vendor's N+1/N+2 convention: an N+K constraint is
// written as min_viable=N, tolerated_losses=K.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/identity.hpp"
#include "mf/domain/window.hpp"

namespace mf {

/// "After this maintenance, the pool must still have at least
/// min_viable + tolerated_losses units of serving capacity."
struct RedundancyRule {
  PoolId pool;
  std::uint32_t min_viable{1};
  std::uint32_t tolerated_losses{0};
  std::string name;

  [[nodiscard]] std::uint32_t required_serving_units() const noexcept {
    return min_viable + tolerated_losses;
  }
};

/// "At most max_out members of this correlated failure domain may be out of
/// service at the same time."
struct DomainLimitRule {
  DomainId domain;
  std::uint32_t max_out{1};
  std::string name;
};

/// "After this maintenance the pool must still have reserve_units of spare
/// serving capacity beyond the redundancy requirement."
struct HeadroomRule {
  PoolId pool;
  std::uint32_t reserve_units{0};
  std::string name;
};

/// A workload or network availability contract: at least min_available of the
/// listed members must be serving.
struct Contract {
  ContractId id;
  std::string name;
  std::vector<TargetId> members;
  std::uint32_t min_available{1};
  bool require_fresh_evidence{true};
};

struct PriorityTier {
  std::uint32_t index{0};
  std::string name;
};

struct Policy {
  std::vector<RedundancyRule> redundancy;
  std::vector<DomainLimitRule> domain_limits;
  std::vector<HeadroomRule> headroom;
  std::vector<Contract> contracts;
  std::vector<Window> windows;
  std::vector<PriorityTier> tiers;

  /// Per-kind evidence time-to-live. Kinds absent from the map use the default.
  std::map<EvidenceKind, Nanos> evidence_ttl;
  /// Sources that must independently report fresh evidence before a start.
  std::uint32_t min_evidence_sources{1};
  bool require_window{false};

  Nanos max_precondition_age{limits::kMaxPreconditionAgeNanos};
  Nanos default_duration{10 * kNanosPerMinute};
  std::uint32_t max_concurrent_jobs{64};
  Nanos authority_lease{limits::kDefaultAuthorityLeaseNanos};
  Nanos drain_lease{limits::kDefaultDrainLeaseNanos};
  std::uint32_t max_drain_failures_before_block{3};
  std::uint32_t max_verification_attempts{3};
  std::uint32_t max_restoration_attempts{3};

  Revision revision{};

  /// Missing rules fall back to a conservative default: the pool must keep at
  /// least one serving unit and tolerate no additional loss. Returned by value
  /// so callers can never hold a reference into a fallback.
  [[nodiscard]] RedundancyRule redundancy_for(PoolId pool) const;
  [[nodiscard]] const HeadroomRule* headroom_for(PoolId pool) const;
  [[nodiscard]] std::vector<const DomainLimitRule*> domain_limits_for(DomainId domain) const;
  /// True when the rule list names this domain explicitly.
  [[nodiscard]] bool has_domain_limit(DomainId domain) const noexcept;
  [[nodiscard]] const Contract* contract(ContractId id) const;
  [[nodiscard]] std::vector<const Contract*> contracts_touching(
      const std::vector<TargetId>& targets) const;
  [[nodiscard]] Nanos ttl_for(EvidenceKind kind) const;
  [[nodiscard]] bool has_ttl(EvidenceKind kind) const;
  /// Rank of a priority tier: 0 is the most urgent. Unknown tiers sort last.
  [[nodiscard]] std::uint32_t tier_rank(std::uint32_t tier_index) const;
  [[nodiscard]] bool known_tier(std::uint32_t tier_index) const;
  [[nodiscard]] const Window* window(WindowId id) const;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::uint64_t digest() const;
};

/// Default rule used when no explicit rule exists for a pool.
[[nodiscard]] RedundancyRule default_redundancy_rule(PoolId pool);

}  // namespace mf
