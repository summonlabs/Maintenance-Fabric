#pragma once

// Benchmark fixtures: a wide, cheap-to-generate fabric so the measured work is
// the runtime's decision making rather than the fixture.

#include <string>

#include "mf/engine/readiness.hpp"
#include "mf/runtime/controller.hpp"

namespace mf::bench {

inline Topology wide_fabric(std::size_t switches) {
  Topology topology;
  DomainRecord site;
  site.id = DomainId{DomainKind::Site, 1};
  site.name = "site";
  (void)topology.add_domain(site);
  for (std::size_t i = 0; i < switches; ++i) {
    const auto rack = DomainId{DomainKind::Rack, static_cast<std::uint32_t>(i + 1)};
    const auto power = DomainId{DomainKind::Power, static_cast<std::uint32_t>(i + 1)};
    (void)topology.add_domain(DomainRecord{rack, "rack", true});
    (void)topology.add_domain(DomainRecord{power, "feed", true});
    TargetRecord sw;
    sw.id = TargetId{TargetKind::Switch, static_cast<std::uint32_t>(i + 1)};
    sw.name = "sw";
    sw.domains = {site.id, rack, power};
    (void)topology.add_target(sw);
    TargetRecord link;
    link.id = TargetId{TargetKind::Link, static_cast<std::uint32_t>(i + 1)};
    link.name = "lag";
    link.parent = sw.id;
    link.domains = {site.id, rack, power};
    (void)topology.add_target(link);
  }
  PoolRecord pool;
  pool.id = PoolId::from_u64(1);
  pool.name = "uplink";
  for (std::size_t i = 0; i < switches; ++i) {
    pool.members.push_back(TargetId{TargetKind::Link, static_cast<std::uint32_t>(i + 1)});
  }
  (void)topology.add_pool(pool);
  DomainRecord zone;
  zone.id = DomainId{DomainKind::Zone, 1};
  zone.name = "zone";
  (void)topology.add_domain(zone);
  return topology;
}

inline Policy wide_policy(std::uint32_t tolerated_losses) {
  Policy policy;
  policy.redundancy.push_back(RedundancyRule{PoolId::from_u64(1), 1, tolerated_losses, "n-plus"});
  policy.tiers = {PriorityTier{0, "emergency"}, PriorityTier{1, "routine"}};
  policy.max_concurrent_jobs = 64;
  policy.authority_lease = 10 * kNanosPerMinute;
  policy.drain_lease = 10 * kNanosPerMinute;
  // A long observation TTL keeps the benchmark focused on decisions instead of
  // re-publishing an entire fabric every iteration.
  // Twenty hours: inside the runtime's 24h bound, and long enough that a whole
  // benchmark run shares one observation set.
  policy.evidence_ttl[EvidenceKind::TargetHealth] = 20 * kNanosPerHour;
  policy.evidence_ttl[EvidenceKind::MaintenanceCompletion] = 1 * kNanosPerHour;
  policy.evidence_ttl[EvidenceKind::DrainLeaseState] = 1 * kNanosPerHour;
  policy.evidence_ttl[EvidenceKind::RestorationCheck] = 1 * kNanosPerHour;
  return policy;
}

/// Deterministic target selection that stays inside the generated fabric.
inline TargetId target_for(std::uint64_t index, std::size_t switches) {
  const std::size_t bound = switches == 0 ? 1 : switches;
  return TargetId{TargetKind::Link, static_cast<std::uint32_t>(index % bound) + 1};
}

inline Status publish_health(Controller& controller, Nanos now) {
  for (const auto& [id, record] : controller.topology().targets()) {
    (void)record;
    EvidenceRecord evidence;
    evidence.key.kind = EvidenceKind::TargetHealth;
    evidence.key.subject = EvidenceSubject::of(id);
    evidence.key.source = SourceId::from_u64(1);
    evidence.revision = Revision::from_u64(static_cast<std::uint64_t>(now / kNanosPerSecond) + 1);
    evidence.observed_at = now;
    evidence.ttl = controller.policy().ttl_for(EvidenceKind::TargetHealth);
    evidence.observed_by = controller.incarnation();
    evidence.payload.health = Health::Healthy;
    const Status observed = controller.observe(evidence);
    if (!observed.ok()) {
      return observed;
    }
  }
  return ok_status();
}

}  // namespace mf::bench
