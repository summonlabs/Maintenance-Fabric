#pragma once

// Shared example scaffolding. Examples are deliberately small and use the same
// public API an embedder would use.

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"

namespace mf::example {

/// Four fabric switches, two links each, in distinct rack and power domains.
inline Topology fabric(std::size_t switches = 4, std::size_t links_per_switch = 2) {
  Topology topology;
  for (std::size_t i = 0; i < switches; ++i) {
    const auto rack = DomainId{DomainKind::Rack, static_cast<std::uint32_t>(i + 1)};
    const auto power = DomainId{DomainKind::Power, static_cast<std::uint32_t>(i + 1)};
    (void)topology.add_domain(DomainRecord{rack, "rack-" + std::to_string(i + 1), true});
    (void)topology.add_domain(DomainRecord{power, "feed-" + std::to_string(i + 1), true});
    TargetRecord sw;
    sw.id = TargetId{TargetKind::Switch, static_cast<std::uint32_t>(i + 1)};
    sw.name = "sw-" + std::to_string(i + 1);
    sw.domains = {rack, power};
    (void)topology.add_target(sw);
    for (std::size_t j = 0; j < links_per_switch; ++j) {
      TargetRecord link;
      link.id = TargetId{TargetKind::Link,
                         static_cast<std::uint32_t>(i * links_per_switch + j + 1)};
      link.name = "lag-" + std::to_string(i + 1) + "-" + std::to_string(j + 1);
      link.parent = sw.id;
      link.domains = {rack, power};
      (void)topology.add_target(link);
    }
  }
  for (std::size_t j = 0; j < links_per_switch; ++j) {
    PoolRecord pool;
    pool.id = PoolId::from_u64(j + 1);
    pool.name = "uplink-" + std::to_string(j + 1);
    for (std::size_t i = 0; i < switches; ++i) {
      pool.members.push_back(
          TargetId{TargetKind::Link, static_cast<std::uint32_t>(i * links_per_switch + j + 1)});
    }
    (void)topology.add_pool(pool);
  }
  return topology;
}

/// N+K redundancy and one-out-per-correlated-domain limits.
inline Policy policy_with(std::uint32_t tolerated_losses, std::size_t pools = 2) {
  Policy policy;
  for (std::size_t j = 0; j < pools; ++j) {
    policy.redundancy.push_back(RedundancyRule{
        PoolId::from_u64(j + 1), 1, tolerated_losses, "n-plus-" + std::to_string(tolerated_losses)});
  }
  for (std::uint32_t i = 1; i <= 8; ++i) {
    policy.domain_limits.push_back(
        DomainLimitRule{DomainId{DomainKind::Rack, i}, 1, "one-per-rack"});
  }
  policy.tiers = {PriorityTier{0, "emergency"}, PriorityTier{1, "routine"}};
  policy.default_duration = 10 * kNanosPerMinute;
  policy.authority_lease = 10 * kNanosPerMinute;
  policy.drain_lease = 10 * kNanosPerMinute;
  return policy;
}

/// Strictly increasing revision source for example-authored observations.
inline std::uint64_t next_evidence_revision() {
  static std::atomic<std::uint64_t> counter{1};
  return counter.fetch_add(1);
}

inline Status publish_health(Controller& controller, Nanos now) {
  for (const auto& [id, record] : controller.topology().targets()) {
    (void)record;
    EvidenceRecord evidence;
    evidence.key.kind = EvidenceKind::TargetHealth;
    evidence.key.subject = EvidenceSubject::of(id);
    evidence.key.source = SourceId::from_u64(1);
    evidence.revision = Revision::from_u64(next_evidence_revision());
    evidence.observed_at = now;
    evidence.ttl = controller.policy().ttl_for(EvidenceKind::TargetHealth);
    evidence.observed_by = controller.incarnation();
    evidence.payload.health = Health::Healthy;
    evidence.payload.result = true;
    const Status observed = controller.observe(evidence);
    if (!observed.ok()) {
      return observed;
    }
  }
  return ok_status();
}

inline std::string scratch_directory(const std::string& name) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("mf-example-" + name);
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

}  // namespace mf::example
