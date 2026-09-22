// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <set>

#include "mf/engine/arbitrator.hpp"
#include "mf/engine/explain.hpp"
#include "mf/engine/impact.hpp"
#include "mf/engine/preconditions.hpp"
#include "mf/engine/scheduler.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

ControllerIncarnation incarnation_of(std::uint64_t epoch = 1, std::uint64_t nonce = 3) {
  ControllerIncarnation incarnation;
  incarnation.controller = ControllerId::from_u64(1);
  incarnation.boot_epoch = BootEpoch::from_u64(epoch);
  incarnation.nonce = nonce;
  return incarnation;
}

/// Health evidence published for every target of a topology.
void publish(const Topology& topology, EvidenceStore& store, const ControllerIncarnation& who,
             Nanos now, Health health, const std::vector<TargetId>& except = {}) {
  for (const auto& [id, record] : topology.targets()) {
    (void)record;
    if (std::find(except.begin(), except.end(), id) != except.end()) {
      continue;
    }
    EvidenceRecord evidence = mftest::health_evidence(id, health, now, 100000);
    evidence.observed_by = who;
    MF_CHECK_OK(store.observe(evidence));
  }
}

CapacityImpact impact_of(const Topology& topology, const Policy& policy, const ReadinessView& view,
                         const std::vector<TargetId>& planned,
                         const std::vector<TargetId>& baseline = {}) {
  ImpactInput input;
  input.topology = &topology;
  input.policy = &policy;
  input.readiness = &view;
  input.planned_removed = planned;
  input.baseline_removed = baseline;
  return compute_impact(input);
}

}  // namespace

MF_TEST(impact, n_plus_one_blocks_when_tolerance_is_already_consumed) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);

  // All four members healthy: removing one leaves three.
  {
    const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);
    const CapacityImpact impact =
        impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
    MF_CHECK(impact.satisfied);
    MF_CHECK_EQ(impact.pools.size(), 1u);
    MF_CHECK_EQ(impact.pools.front().remaining_units, 3u);
    MF_CHECK_EQ(impact.pools.front().required_units, 2u);
  }

  // Two members already down: destroying one more unit would leave the pool
  // below the N+1 requirement, so maintenance is refused.
  // (link:3 and link:5 are the second and third members of pool 1.)
  {
    EvidenceStore degraded;
    publish(topology, degraded, who, 1000, Health::Healthy,
            {TargetId{TargetKind::Link, 3}, TargetId{TargetKind::Link, 5}});
    for (const std::uint32_t index : {3u, 5u}) {
      EvidenceRecord record =
          mftest::health_evidence(TargetId{TargetKind::Link, index}, Health::Down, 1000, 100000);
      record.observed_by = who;
      MF_CHECK_OK(degraded.observe(record));
    }
    const ReadinessView view = ReadinessView::build(topology, degraded, policy, who, 1000);
    const CapacityImpact impact =
        impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
    MF_CHECK(!impact.satisfied);
    MF_CHECK_EQ(impact.block_reason().value_or(BlockReason::None),
                BlockReason::RedundancyViolation);
    MF_CHECK_EQ(impact.pools.front().remaining_units, 1u);
  }

  // Two other jobs already hold the same tolerance budget: unsafe.
  {
    const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);
    const CapacityImpact impact =
        impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}},
                  {TargetId{TargetKind::Link, 3}, TargetId{TargetKind::Link, 5}});
    MF_CHECK(!impact.satisfied);
  }

  // The classical N+1 reading: a healthy 4-member pool may give up one unit and
  // still keep N+1 serving, and may give up two.
  {
    const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);
    const CapacityImpact two = impact_of(
        topology, policy, view, {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 3}});
    MF_CHECK(two.satisfied);
    MF_CHECK_EQ(two.pools.front().remaining_units, 2u);
  }

  // A pool whose only members are already at the boundary refuses maintenance.
  {
    Topology pair;
    MF_CHECK_OK(pair.add_domain(DomainRecord{DomainId{DomainKind::Rack, 1}, "rack-1", true}));
    MF_CHECK_OK(pair.add_domain(DomainRecord{DomainId{DomainKind::Rack, 2}, "rack-2", true}));
    for (std::uint32_t i = 1; i <= 2; ++i) {
      TargetRecord record;
      record.id = TargetId{TargetKind::Link, i};
      record.domains = {DomainId{DomainKind::Rack, i}};
      MF_CHECK_OK(pair.add_target(record));
    }
    MF_CHECK_OK(pair.add_pool(PoolRecord{PoolId::from_u64(1), "pair",
                                         {TargetId{TargetKind::Link, 1},
                                          TargetId{TargetKind::Link, 2}}}));
    EvidenceStore pair_store;
    publish(pair, pair_store, who, 1000, Health::Healthy);
    const ReadinessView pair_view = ReadinessView::build(pair, pair_store, policy, who, 1000);
    const CapacityImpact boundary =
        impact_of(pair, policy, pair_view, {TargetId{TargetKind::Link, 1}});
    MF_CHECK(!boundary.satisfied);
    MF_CHECK_EQ(boundary.pools.front().required_units, 2u);
    MF_CHECK_EQ(boundary.pools.front().remaining_units, 1u);
  }
}

MF_TEST(impact, n_plus_two_is_expressible_as_generic_policy) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(2);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy, {TargetId{TargetKind::Link, 3}});
  EvidenceRecord down =
      mftest::health_evidence(TargetId{TargetKind::Link, 3}, Health::Down, 1000, 100000);
  down.observed_by = who;
  MF_CHECK_OK(store.observe(down));

  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);
  // N+2 requires three serving units; one is down and one would be removed.
  const CapacityImpact blocked =
      impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
  MF_CHECK(!blocked.satisfied);
  MF_CHECK_EQ(blocked.pools.front().required_units, 3u);
  MF_CHECK_EQ(blocked.pools.front().remaining_units, 2u);

  // A clean pool of four still satisfies N+2 for a single removal.
  EvidenceStore clean;
  publish(topology, clean, who, 1000, Health::Healthy);
  const ReadinessView clean_view = ReadinessView::build(topology, clean, policy, who, 1000);
  MF_CHECK(impact_of(topology, policy, clean_view, std::vector<TargetId>{TargetId{TargetKind::Link, 1}})
               .satisfied);
  MF_CHECK(!impact_of(topology, policy, clean_view,
                      {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 2}})
                .satisfied);
}

MF_TEST(impact, unknown_health_never_counts_toward_capacity) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy,
          {TargetId{TargetKind::Link, 3}, TargetId{TargetKind::Link, 5}});

  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);
  const CapacityImpact impact = impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
  MF_CHECK(!impact.satisfied);
  MF_CHECK_EQ(impact.pools.front().unknown_units, 2u);
  MF_CHECK_EQ(impact.pools.front().serving_units, 2u);
  MF_CHECK_EQ(impact.pools.front().remaining_units, 1u);
}

MF_TEST(impact, correlated_domain_limits_are_generic_policy) {
  Topology topology;
  MF_CHECK_OK(topology.add_domain(DomainRecord{DomainId{DomainKind::Rack, 1}, "rack-1", true}));
  for (std::uint32_t i = 1; i <= 3; ++i) {
    TargetRecord record;
    record.id = TargetId{TargetKind::Link, i};
    record.name = "link-" + std::to_string(i);
    record.domains = {DomainId{DomainKind::Rack, 1}};
    MF_CHECK_OK(topology.add_target(record));
  }
  MF_CHECK_OK(topology.add_pool(
      PoolRecord{PoolId::from_u64(1), "pool", {TargetId{TargetKind::Link, 1},
                                               TargetId{TargetKind::Link, 2},
                                               TargetId{TargetKind::Link, 3}}}));
  Policy policy;
  policy.redundancy.push_back(RedundancyRule{PoolId::from_u64(1), 1, 1, "n-plus-1"});
  policy.domain_limits.push_back(
      DomainLimitRule{DomainId{DomainKind::Rack, 1}, 1, "one-per-rack"});

  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  // One member out: allowed by both rules.
  const CapacityImpact single =
      impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
  MF_CHECK(single.satisfied);
  MF_CHECK_EQ(single.domains.front().out_after, 1u);

  // Two members of the same correlated domain: the domain rule refuses it even
  // though the pool alone would tolerate one loss.
  const CapacityImpact double_removal = impact_of(
      topology, policy, view, {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 2}});
  MF_CHECK(!double_removal.satisfied);
  MF_CHECK_EQ(double_removal.domains.front().out_after, 2u);
  MF_CHECK_EQ(double_removal.domains.front().max_out, 1u);

  // A plan that removes two members of a limited domain and a third one is
  // reported by every governed rule with an explicit reason.
  const CapacityImpact three = impact_of(topology, policy, view,
                                         {TargetId{TargetKind::Link, 1},
                                          TargetId{TargetKind::Link, 2},
                                          TargetId{TargetKind::Link, 3}});
  MF_CHECK(!three.satisfied);
  MF_CHECK(!three.first_violation().empty());
}

MF_TEST(impact, capacity_headroom_and_contracts) {
  const Topology topology = mftest::make_topology();
  Policy policy = mftest::make_policy(1);
  policy.headroom.push_back(HeadroomRule{PoolId::from_u64(1), 2, "reserve-two"});
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  const CapacityImpact reserved =
      impact_of(topology, policy, view, {TargetId{TargetKind::Link, 1}});
  MF_CHECK(!reserved.satisfied);
  MF_CHECK_EQ(reserved.pools.front().reserve_units, 2u);
  MF_CHECK_EQ(reserved.pools.front().remaining_units, 3u);

  policy.headroom.clear();
  // The contract requires link:1 or link:2 to stay available.
  const CapacityImpact contract = impact_of(
      topology, policy, view, {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 2}});
  MF_CHECK(!contract.satisfied);
  MF_CHECK_EQ(contract.contracts.size(), 1u);
  MF_CHECK_EQ(contract.contracts.front().available_after, 0u);
  MF_CHECK_EQ(contract.contracts.front().min_available, 1u);
}

MF_TEST(impact, unknown_targets_and_unmaintainable_targets_are_reported) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  const CapacityImpact missing =
      impact_of(topology, policy, view, {TargetId{TargetKind::Pod, 77}});
  MF_CHECK(!missing.satisfied);
  MF_CHECK_EQ(missing.block_reason(), BlockReason::TargetUnknown);
}

MF_TEST(preconditions, every_kind_is_reported) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  MaintenanceJob job;
  job.id = JobId::from_u64(1);
  job.generation = GenerationId::from_u64(1);
  job.revision = Revision::from_u64(1);
  job.title = "swap optics";
  job.reason = "planned";
  job.targets = {TargetId{TargetKind::Link, 1}};
  job.removal_set = {TargetId{TargetKind::Link, 1}};
  job.requestor = requestor_from_name("test");
  job.requires_drain = true;

  JobTable jobs;
  MF_CHECK_OK(jobs.insert(job));
  AuthorityLedger ledger(1000);

  PreconditionContext context;
  context.job = &job;
  context.topology = &topology;
  context.policy = &policy;
  context.evidence = &store;
  context.ledger = &ledger;
  context.jobs = &jobs;
  context.readiness = &view;
  context.current = who;
  context.now = 1000;

  const PreconditionSet planning = evaluate_preconditions(context);
  MF_CHECK_EQ(planning.violated, 0u);
  MF_CHECK(planning.pending >= 2u);
  MF_CHECK(!planning.all_satisfied);
  MF_CHECK(planning.find(PreconditionKind::Redundancy)->status == PreconditionStatus::Satisfied);
  MF_CHECK(planning.find(PreconditionKind::EvidenceFresh)->status == PreconditionStatus::Satisfied);
  MF_CHECK(planning.find(PreconditionKind::AuthorityReserved)->status == PreconditionStatus::Pending);
  MF_CHECK(planning.find(PreconditionKind::DrainAcquired)->status == PreconditionStatus::Pending);

  // With authority and drain asserted but not actually held, both are pending.
  context.check_authority = true;
  context.check_drain = true;
  const PreconditionSet without = evaluate_preconditions(context);
  MF_CHECK(without.find(PreconditionKind::AuthorityReserved)->status ==
           PreconditionStatus::Pending);
  MF_CHECK(without.find(PreconditionKind::DrainAcquired)->status == PreconditionStatus::Pending);
  MF_CHECK(without.pending >= 2u);
}

MF_TEST(preconditions, stale_evidence_blocks_admission) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  for (const auto& [id, record] : topology.targets()) {
    (void)record;
    EvidenceRecord evidence = mftest::health_evidence(id, Health::Healthy, 1000, 100);
    evidence.observed_by = who;
    MF_CHECK_OK(store.observe(evidence));
  }
  MaintenanceJob job;
  job.id = JobId::from_u64(1);
  job.generation = GenerationId::from_u64(1);
  job.title = "swap optics";
  job.targets = {TargetId{TargetKind::Link, 1}};
  job.removal_set = {TargetId{TargetKind::Link, 1}};
  job.requestor = requestor_from_name("test");
  JobTable jobs;
  MF_CHECK_OK(jobs.insert(job));
  AuthorityLedger ledger(1000);

  const ReadinessView fresh_view = ReadinessView::build(topology, store, policy, who, 1000);
  const ReadinessView stale_view = ReadinessView::build(topology, store, policy, who, 5000);

  PreconditionContext context;
  context.job = &job;
  context.topology = &topology;
  context.policy = &policy;
  context.evidence = &store;
  context.ledger = &ledger;
  context.jobs = &jobs;
  context.readiness = &fresh_view;
  context.current = who;
  context.now = 1000;
  MF_CHECK_EQ(evaluate_preconditions(context).violated, 0u);

  context.readiness = &stale_view;
  context.now = 5000;
  const PreconditionSet stale = evaluate_preconditions(context);
  MF_CHECK(stale.violated >= 1u);
  MF_CHECK_EQ(stale.first_violation(), BlockReason::EvidenceMissingOrStale);
  MF_CHECK(stale.find(PreconditionKind::EvidenceFresh)->status == PreconditionStatus::Violated);
}

MF_TEST(preconditions, control_plane_requires_quorum_evidence) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);

  MaintenanceJob job;
  job.id = JobId::from_u64(1);
  job.generation = GenerationId::from_u64(1);
  job.title = "control plane restart";
  job.targets = {TargetId{TargetKind::ControlPlane, 1}};
  job.removal_set = {TargetId{TargetKind::ControlPlane, 1}};
  job.requestor = requestor_from_name("test");
  JobTable jobs;
  MF_CHECK_OK(jobs.insert(job));
  AuthorityLedger ledger(1000);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  PreconditionContext context;
  context.job = &job;
  context.topology = &topology;
  context.policy = &policy;
  context.evidence = &store;
  context.ledger = &ledger;
  context.jobs = &jobs;
  context.readiness = &view;
  context.current = who;
  context.now = 1000;
  const PreconditionSet without_quorum = evaluate_preconditions(context);
  MF_CHECK_EQ(without_quorum.first_violation(), BlockReason::ControlPlaneUnsafe);

  EvidenceRecord quorum;
  quorum.key.kind = EvidenceKind::ControlPlaneQuorum;
  quorum.key.subject = EvidenceSubject::global();
  quorum.key.source = SourceId::from_u64(1);
  quorum.id = EvidenceId::from_u64(1);
  quorum.revision = Revision::from_u64(1);
  quorum.observed_at = 1000;
  quorum.ttl = 100000;
  quorum.observed_by = who;
  quorum.payload.result = true;
  MF_CHECK_OK(store.observe(quorum));
  const PreconditionSet with_quorum = evaluate_preconditions(context);
  MF_CHECK(with_quorum.find(PreconditionKind::ControlPlaneQuorum)->status ==
           PreconditionStatus::Satisfied);
  for (const PreconditionResult& result : with_quorum.results) {
    if (result.status != PreconditionStatus::Satisfied &&
        result.kind != PreconditionKind::AuthorityReserved &&
        result.kind != PreconditionKind::DrainAcquired) {
      MF_FAIL("unexpected precondition " + std::string(to_string(result.status)) + " for " +
              std::string(to_string(result.kind)) + ": " + result.detail);
    }
  }
}

MF_TEST(arbitrator, ordering_is_a_total_deterministic_order) {
  Policy policy = mftest::make_policy(1);
  policy.tiers = {PriorityTier{0, "emergency"}, PriorityTier{1, "routine"}, PriorityTier{2, "batch"}};
  JobTable jobs;
  const auto make = [&jobs](std::uint64_t id, std::uint32_t tier, Nanos created) {
    MaintenanceJob job;
    job.id = JobId::from_u64(id);
    job.generation = GenerationId::from_u64(1);
    job.revision = Revision::from_u64(1);
    job.title = "job " + std::to_string(id);
    job.targets = {TargetId{TargetKind::Link, static_cast<std::uint32_t>(id)}};
    job.removal_set = job.targets;
    job.priority = tier;
    job.created_at = created;
    MF_CHECK_OK(jobs.insert(job));
  };
  make(3, 2, 100);
  make(1, 1, 500);
  make(2, 1, 100);
  make(4, 0, 900);

  std::vector<const MaintenanceJob*> all;
  for (const auto& [id, job] : jobs.jobs()) {
    (void)id;
    all.push_back(&job);
  }
  const std::vector<ArbitrationCandidate> ordered = order_candidates(all, policy);
  MF_CHECK_EQ(ordered.size(), 4u);
  MF_CHECK_EQ(ordered[0].job.value(), 4u);
  MF_CHECK_EQ(ordered[1].job.value(), 2u);
  MF_CHECK_EQ(ordered[2].job.value(), 1u);
  MF_CHECK_EQ(ordered[3].job.value(), 3u);
  const std::vector<ArbitrationCandidate> again = order_candidates(all, policy);
  MF_CHECK_EQ(ordered[0].ordering_key, again[0].ordering_key);
}

MF_TEST(arbitrator, overlapping_jobs_cannot_both_remove_correlated_capacity) {
  const Topology topology = mftest::make_topology();
  // N+2 requires three serving units, so each job alone is admissible on a
  // healthy four-member pool while the pair is not.
  const Policy policy = mftest::make_policy(2);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  JobTable jobs;
  const auto add = [&jobs](std::uint64_t id, TargetId target, Nanos created) {
    MaintenanceJob job;
    job.id = JobId::from_u64(id);
    job.generation = GenerationId::from_u64(1);
    job.revision = Revision::from_u64(1);
    job.title = "job";
    job.targets = {target};
    job.removal_set = {target};
    job.active_attempt = AttemptId{job.id, job.generation, AttemptOrdinal::from_u64(1)};
    job.created_at = created;
    job.priority = 0;
    MF_CHECK_OK(jobs.insert(job));
  };
  // Both jobs are individually admissible under N+2; together they would leave
  // only two serving units, which the same policy forbids.
  // link:1 and link:3 are both members of pool 1 but live in different racks,
  // so only the redundancy rule can refuse them together.
  add(1, TargetId{TargetKind::Link, 1}, 100);
  add(2, TargetId{TargetKind::Link, 3}, 200);

  AuthorityLedger ledger(policy.authority_lease);
  ArbitrationContext context;
  context.topology = &topology;
  context.policy = &policy;
  context.readiness = &view;
  context.ledger = &ledger;
  context.jobs = &jobs;
  context.candidates = {JobId::from_u64(1), JobId::from_u64(2)};
  context.current = who;
  context.now = 1000;

  const ArbitrationReport report = arbitrate(context);
  MF_CHECK_EQ(report.granted, 1u);
  MF_CHECK_EQ(report.denied, 1u);
  MF_CHECK_EQ(report.decisions[0].outcome, ArbitrationOutcome::Granted);
  MF_CHECK_EQ(report.decisions[1].outcome, ArbitrationOutcome::Denied);
  if (report.decisions[1].reason != BlockReason::RedundancyViolation) {
    MF_FAIL("unexpected denial: " + report.decisions[1].detail);
  }
  MF_CHECK(report.decisions[1].detail.find("policy requires") != std::string::npos);

  // Re-running does not double-grant and produces the same decision shape.
  const ArbitrationReport repeat = arbitrate(context);
  MF_CHECK_EQ(repeat.decisions.size(), 2u);
  MF_CHECK_EQ(repeat.decisions[0].outcome, ArbitrationOutcome::Deferred);
  MF_CHECK_EQ(repeat.decisions[0].authority, report.decisions[0].authority);
}

MF_TEST(arbitrator, concurrency_bound_is_enforced) {
  const Topology topology = mftest::make_topology();
  Policy policy = mftest::make_policy(1);
  policy.max_concurrent_jobs = 1;
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  JobTable jobs;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    MaintenanceJob job;
    job.id = JobId::from_u64(id);
    job.generation = GenerationId::from_u64(1);
    job.title = "job";
    // Distinct racks so the pool/domain rules alone would allow all three.
    job.targets = {TargetId{TargetKind::Link, static_cast<std::uint32_t>(id == 1 ? 1 : (id == 2 ? 3 : 5))}};
    job.removal_set = job.targets;
    job.active_attempt = AttemptId{job.id, job.generation, AttemptOrdinal::from_u64(1)};
    job.created_at = static_cast<Nanos>(id);
    MF_CHECK_OK(jobs.insert(job));
  }
  AuthorityLedger ledger(policy.authority_lease);
  ArbitrationContext context;
  context.topology = &topology;
  context.policy = &policy;
  context.readiness = &view;
  context.ledger = &ledger;
  context.jobs = &jobs;
  context.candidates = {JobId::from_u64(1), JobId::from_u64(2), JobId::from_u64(3)};
  context.current = who;
  context.now = 1000;
  const ArbitrationReport report = arbitrate(context);
  MF_CHECK_EQ(report.granted, 1u);
  MF_CHECK_EQ(report.denied, 2u);
  MF_CHECK(report.decisions[1].detail.find("concurrency limit") != std::string::npos);
}

MF_TEST(scheduler, classification_is_deterministic) {
  JobTable jobs;
  const auto add = [&jobs](std::uint64_t id, JobState state) {
    MaintenanceJob job;
    job.id = JobId::from_u64(id);
    job.generation = GenerationId::from_u64(1);
    job.state = state;
    job.title = "job";
    MF_CHECK_OK(jobs.insert(job));
  };
  add(5, JobState::Validated);
  add(3, JobState::Ready);
  add(1, JobState::InMaintenance);
  add(2, JobState::Verifying);
  add(4, JobState::Blocked);
  add(6, JobState::Restoring);
  add(7, JobState::Complete);

  const Policy policy = mftest::make_policy(1);
  SchedulerInputs inputs;
  inputs.jobs = &jobs;
  inputs.policy = &policy;
  inputs.now = 500;
  const SchedulerSelection selection = plan_tick(inputs);
  MF_CHECK_EQ(selection.validated.size(), 1u);
  MF_CHECK_EQ(selection.validated.front().value(), 5u);
  MF_CHECK_EQ(selection.in_maintenance.size(), 1u);
  MF_CHECK_EQ(selection.verifying.size(), 1u);
  MF_CHECK_EQ(selection.restoring.size(), 1u);
  MF_CHECK_EQ(selection.blocked.size(), 1u);
  MF_CHECK_EQ(selection.overrun.size(), 0u);
  MF_CHECK(std::is_sorted(selection.runnable.begin(), selection.runnable.end()));
  MF_CHECK_EQ(selection.digest, plan_tick(inputs).digest);
}

MF_TEST(scheduler, window_close_marks_overrun) {
  JobTable jobs;
  MaintenanceJob job;
  job.id = JobId::from_u64(1);
  job.generation = GenerationId::from_u64(1);
  job.state = JobState::InMaintenance;
  job.service_removed = true;
  job.window_closes_at = 100;
  MF_CHECK_OK(jobs.insert(job));
  const Policy policy = mftest::make_policy(1);
  SchedulerInputs inputs;
  inputs.jobs = &jobs;
  inputs.policy = &policy;
  inputs.now = 100;
  MF_CHECK_EQ(plan_tick(inputs).overrun.size(), 1u);
  inputs.now = 99;
  MF_CHECK_EQ(plan_tick(inputs).overrun.size(), 0u);
}

MF_TEST(explain, identical_inputs_produce_identical_explanations) {
  const Topology topology = mftest::make_topology();
  const Policy policy = mftest::make_policy(1);
  const ControllerIncarnation who = incarnation_of();
  EvidenceStore store;
  publish(topology, store, who, 1000, Health::Healthy);
  const ReadinessView view = ReadinessView::build(topology, store, policy, who, 1000);

  MaintenanceJob job;
  job.id = JobId::from_u64(1);
  job.generation = GenerationId::from_u64(2);
  job.revision = Revision::from_u64(6);
  job.title = "swap optics";
  job.targets = {TargetId{TargetKind::Link, 1}};
  job.removal_set = {TargetId{TargetKind::Link, 1}};
  job.priority = 1;
  job.requestor = requestor_from_name("test");
  JobTable jobs;
  MF_CHECK_OK(jobs.insert(job));
  AuthorityLedger ledger(1000);

  const auto build = [&](const Policy& active) {
    ExplainContext context;
    context.job = &job;
    context.topology = &topology;
    context.policy = &active;
    context.evidence = &store;
    context.ledger = &ledger;
    context.jobs = &jobs;
    context.readiness = &view;
    context.current = who;
    context.now = 1000;
    context.action = "admission";
    return explain_job(context);
  };
  const Explanation first = build(policy);
  const Explanation second = build(policy);
  MF_CHECK_EQ(first.to_text(), second.to_text());
  MF_CHECK_EQ(first.digest, second.digest);
  MF_CHECK(first.to_text().find("explanation digest=") != std::string::npos);
  MF_CHECK(first.to_text().find("policy:") != std::string::npos);
  MF_CHECK(first.to_text().find("rejected alternatives") != std::string::npos);

  Policy changed = policy;
  changed.min_evidence_sources = 2;
  const Explanation other = build(changed);
  MF_CHECK_NE(other.digest, first.digest);
}

MF_TEST_MAIN()
