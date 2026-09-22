#pragma once

// Declared topology: targets, their containment tree, failure-domain membership
// and redundancy pools. The topology is static configuration plus operator
// declarations; it carries no operational health.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/domain/target.hpp"

namespace mf {

struct DomainRecord {
  DomainId id;
  std::string name;
  /// True when the domain models a correlated failure: every member can fail
  /// for one shared reason.
  bool correlated{true};
};

struct PoolRecord {
  PoolId id;
  std::string name;
  std::vector<TargetId> members;
};

class Topology {
 public:
  Status add_domain(DomainRecord record);
  Status add_target(TargetRecord record);
  Status add_pool(PoolRecord record);

  [[nodiscard]] const TargetRecord* target(TargetId id) const noexcept;
  [[nodiscard]] const DomainRecord* domain(DomainId id) const noexcept;
  [[nodiscard]] const PoolRecord* pool(PoolId id) const noexcept;

  [[nodiscard]] const std::map<TargetId, TargetRecord>& targets() const noexcept {
    return targets_;
  }
  [[nodiscard]] const std::map<DomainId, DomainRecord>& domains() const noexcept {
    return domains_;
  }
  [[nodiscard]] const std::map<PoolId, PoolRecord>& pools() const noexcept { return pools_; }

  [[nodiscard]] std::vector<TargetId> domain_members(DomainId id) const;
  [[nodiscard]] std::vector<PoolId> pools_containing(TargetId id) const;
  [[nodiscard]] std::vector<TargetId> children(TargetId id) const;
  [[nodiscard]] std::vector<TargetId> ancestors(TargetId id) const;
  /// Target and every descendant, ascending.
  [[nodiscard]] std::vector<TargetId> subtree(TargetId id) const;
  /// Union of the subtrees of p requested, ascending and de-duplicated. This is
  /// the set of targets physically taken out of service by a maintenance job.
  [[nodiscard]] std::vector<TargetId> removal_set(const std::vector<TargetId>& requested) const;
  /// Domains touched by any target in p targets, ascending.
  [[nodiscard]] std::vector<DomainId> domains_of(const std::vector<TargetId>& targets) const;
  /// Pools that contain at least one member of p targets, ascending.
  [[nodiscard]] std::vector<PoolId> pools_of(const std::vector<TargetId>& targets) const;

  [[nodiscard]] bool contains(TargetId id) const noexcept { return target(id) != nullptr; }

  [[nodiscard]] Revision revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t digest() const;
  [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }

  /// Consistency check: pool members exist, parent chains are acyclic, limits
  /// respected. Called after loading a topology file.
  [[nodiscard]] Status validate() const;

 private:
  void touch() noexcept;

  std::map<TargetId, TargetRecord> targets_;
  std::map<DomainId, DomainRecord> domains_;
  std::map<PoolId, PoolRecord> pools_;
  std::map<DomainId, std::vector<TargetId>> domain_members_;
  std::map<TargetId, std::vector<PoolId>> target_pools_;
  std::map<TargetId, std::vector<TargetId>> children_;
  Revision revision_{};
};

}  // namespace mf
