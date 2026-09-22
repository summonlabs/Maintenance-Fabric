// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <set>

#include "mf/domain/authority.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/lifecycle.hpp"
#include "mf/domain/window.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

MF_TEST(topology, structure_and_expansion) {
  const Topology topology = mftest::make_topology();
  MF_CHECK_OK(topology.validate());
  MF_CHECK_EQ(topology.targets().size(), 14u);
  MF_CHECK_EQ(topology.pools().size(), 2u);

  const std::vector<TargetId> subtree = topology.subtree(TargetId{TargetKind::Switch, 1});
  MF_CHECK_EQ(subtree.size(), 3u);
  const std::vector<TargetId> removal =
      topology.removal_set({TargetId{TargetKind::Switch, 1}, TargetId{TargetKind::Switch, 1}});
  MF_CHECK_EQ(removal.size(), 3u);
  MF_CHECK(std::is_sorted(removal.begin(), removal.end()));

  const std::vector<PoolId> pools = topology.pools_of({TargetId{TargetKind::Link, 1}});
  MF_CHECK_EQ(pools.size(), 1u);
  MF_CHECK_EQ(pools.front().value(), 1u);
  const std::vector<DomainId> domains = topology.domains_of({TargetId{TargetKind::Link, 1}});
  MF_CHECK_EQ(domains.size(), 4u);
}

MF_TEST(topology, rejects_inconsistent_input) {
  Topology topology;
  MF_CHECK_OK(topology.add_domain(DomainRecord{DomainId{DomainKind::Rack, 1}, "rack", true}));
  // Unknown domain.
  TargetRecord orphan;
  orphan.id = TargetId{TargetKind::Link, 1};
  orphan.domains = {DomainId{DomainKind::Rack, 9}};
  MF_CHECK_ERR(topology.add_target(orphan), ErrorCode::NotFound);
  // Unknown parent.
  TargetRecord child;
  child.id = TargetId{TargetKind::Link, 1};
  child.parent = TargetId{TargetKind::Switch, 4};
  child.domains = {DomainId{DomainKind::Rack, 1}};
  MF_CHECK_ERR(topology.add_target(child), ErrorCode::NotFound);
  // Self parent.
  TargetRecord selfish;
  selfish.id = TargetId{TargetKind::Link, 1};
  selfish.parent = selfish.id;
  selfish.domains = {DomainId{DomainKind::Rack, 1}};
  MF_CHECK_ERR(topology.add_target(selfish), ErrorCode::InvalidArgument);
  // Duplicate domain on one target.
  TargetRecord duplicated;
  duplicated.id = TargetId{TargetKind::Link, 2};
  duplicated.domains = {DomainId{DomainKind::Rack, 1}, DomainId{DomainKind::Rack, 1}};
  MF_CHECK_ERR(topology.add_target(duplicated), ErrorCode::InvalidArgument);
  // Zero capacity.
  TargetRecord empty;
  empty.id = TargetId{TargetKind::Link, 3};
  empty.domains = {DomainId{DomainKind::Rack, 1}};
  empty.capacity_units = 0;
  MF_CHECK_ERR(topology.add_target(empty), ErrorCode::InvalidArgument);
}

MF_TEST(topology, revision_advances_and_digest_is_stable) {
  Topology topology = mftest::make_topology();
  const Revision before = topology.revision();
  const std::uint64_t digest = topology.digest();
  MF_CHECK_EQ(topology.digest(), digest);
  MF_CHECK_OK(topology.add_domain(DomainRecord{DomainId{DomainKind::Custom, 77}, "extra", true}));
  MF_CHECK(topology.revision() > before);
  MF_CHECK_NE(topology.digest(), digest);
}

MF_TEST(policy, conservative_default_and_tiers) {
  Policy policy;
  MF_CHECK_OK(policy.validate());
  const RedundancyRule rule = policy.redundancy_for(PoolId::from_u64(3));
  MF_CHECK_EQ(rule.min_viable, 1u);
  MF_CHECK_EQ(rule.tolerated_losses, 0u);
  MF_CHECK_EQ(rule.required_serving_units(), 1u);

  policy.tiers = {PriorityTier{4, "low"}, PriorityTier{0, "high"}, PriorityTier{2, "medium"}};
  MF_CHECK_EQ(policy.tier_rank(0), 0u);
  MF_CHECK_EQ(policy.tier_rank(2), 1u);
  MF_CHECK_EQ(policy.tier_rank(4), 2u);
  MF_CHECK_EQ(policy.tier_rank(99), 3u);
  MF_CHECK(policy.known_tier(2));
  MF_CHECK(!policy.known_tier(3));
  MF_CHECK_OK(policy.validate());

  Policy invalid;
  invalid.min_evidence_sources = 0;
  MF_CHECK_ERR(invalid.validate(), ErrorCode::InvalidArgument);
  Policy overflow;
  overflow.redundancy.push_back(
      RedundancyRule{PoolId::from_u64(1), 0xFFFFFFFFu, 0xFFFFFFFFu, "overflow"});
  MF_CHECK_ERR(overflow.validate(), ErrorCode::Overflow);
}

MF_TEST(evidence, freshness_states) {
  ControllerIncarnation incarnation;
  incarnation.controller = ControllerId::from_u64(1);
  incarnation.boot_epoch = BootEpoch::from_u64(1);
  incarnation.nonce = 77;

  EvidenceStore store;
  const TargetId target{TargetKind::Link, 1};
  EvidenceQuery query;
  query.key.kind = EvidenceKind::TargetHealth;
  query.key.subject = EvidenceSubject::of(target);
  query.key.source = SourceId::from_u64(1);
  query.current = incarnation;
  query.now = 1000;

  MF_CHECK_EQ(to_string(store.evaluate(query).state), std::string("missing"));

  EvidenceRecord record = mftest::health_evidence(target, Health::Healthy, 1000, 100);
  record.observed_by = incarnation;
  MF_CHECK_OK(store.observe(record));
  MF_CHECK(store.evaluate(query).fresh());

  query.now = 1200;
  MF_CHECK_EQ(to_string(store.evaluate(query).state), std::string("expired"));

  query.now = 1000;
  MF_CHECK(store.evaluate(query).fresh());
  query.now = 1000 - 10 * kNanosPerSecond;
  MF_CHECK_EQ(to_string(store.evaluate(query).state), std::string("future-dated"));
}

MF_TEST(evidence, restart_makes_volatile_evidence_stale) {
  ControllerIncarnation first;
  first.controller = ControllerId::from_u64(1);
  first.boot_epoch = BootEpoch::from_u64(1);
  first.nonce = 11;
  ControllerIncarnation second = first;
  second.boot_epoch = BootEpoch::from_u64(2);
  second.nonce = 22;

  EvidenceStore store;
  const TargetId target{TargetKind::Switch, 2};
  EvidenceRecord record = mftest::health_evidence(target, Health::Healthy, 5000, 100000);
  record.observed_by = first;
  MF_CHECK_OK(store.observe(record));

  EvidenceQuery query;
  query.key = record.key;
  query.current = first;
  query.now = 5100;
  MF_CHECK(store.evaluate(query).fresh());

  query.current = second;
  const Freshness after_restart = store.evaluate(query);
  MF_CHECK(!after_restart.fresh());
  MF_CHECK_EQ(to_string(after_restart.state), std::string("foreign-incarnation"));
}

MF_TEST(evidence, rejects_replays_and_bounds_capacity) {
  ControllerIncarnation incarnation;
  incarnation.controller = ControllerId::from_u64(1);
  incarnation.boot_epoch = BootEpoch::from_u64(1);
  incarnation.nonce = 5;
  EvidenceStore store(4);
  EvidenceRecord record = mftest::health_evidence(TargetId{TargetKind::Link, 1}, Health::Healthy, 10, 100);
  record.observed_by = incarnation;
  record.revision = Revision::from_u64(5);
  MF_CHECK_OK(store.observe(record));

  EvidenceRecord replay = record;
  replay.revision = Revision::from_u64(5);
  MF_CHECK_ERR(store.observe(replay), ErrorCode::StaleRevision);
  replay.revision = Revision::from_u64(4);
  MF_CHECK_ERR(store.observe(replay), ErrorCode::StaleRevision);
  replay.revision = Revision::from_u64(6);
  MF_CHECK_OK(store.observe(replay));

  // Capacity is enforced: a full store refuses an unrelated fresh key until an
  // expired record frees space.
  for (std::uint32_t i = 2; i <= 4; ++i) {
    EvidenceRecord extra =
        mftest::health_evidence(TargetId{TargetKind::Link, i}, Health::Healthy, 10, 100);
    extra.observed_by = incarnation;
    MF_CHECK_OK(store.observe(extra));
  }
  EvidenceRecord overflow =
      mftest::health_evidence(TargetId{TargetKind::Link, 9}, Health::Healthy, 10, 100);
  overflow.observed_by = incarnation;
  MF_CHECK_ERR(store.observe(overflow), ErrorCode::LimitExceeded);
  MF_CHECK_EQ(store.size(), 4u);
  MF_CHECK_EQ(store.rejections(), 3u);

  EvidenceRecord later =
      mftest::health_evidence(TargetId{TargetKind::Link, 9}, Health::Healthy, 1000, 100);
  later.observed_by = incarnation;
  MF_CHECK_OK(store.observe(later));
  MF_CHECK(store.evictions() > 0);
}

MF_TEST(evidence, revision_regression_in_observation_time_is_rejected) {
  ControllerIncarnation incarnation;
  incarnation.controller = ControllerId::from_u64(1);
  incarnation.boot_epoch = BootEpoch::from_u64(1);
  incarnation.nonce = 9;
  EvidenceStore store;
  EvidenceRecord record = mftest::health_evidence(TargetId{TargetKind::Link, 1}, Health::Healthy, 100, 1000);
  record.observed_by = incarnation;
  MF_CHECK_OK(store.observe(record));
  EvidenceRecord regressed = record;
  regressed.revision = Revision::from_u64(record.revision.value() + 1);
  regressed.observed_at = 50;
  MF_CHECK_ERR(store.observe(regressed), ErrorCode::StaleEvidence);
}

MF_TEST(evidence, malformed_records_are_rejected) {
  EvidenceStore store;
  EvidenceRecord empty;
  MF_CHECK_ERR(store.observe(empty), ErrorCode::InvalidArgument);
  EvidenceRecord no_source = empty;
  no_source.id = EvidenceId::from_u64(1);
  no_source.revision = Revision::from_u64(1);
  no_source.key.subject = EvidenceSubject::of(TargetId{TargetKind::Link, 1});
  no_source.observed_by = ControllerIncarnation{ControllerId::from_u64(1), BootEpoch::from_u64(1), 3};
  no_source.ttl = 10;
  MF_CHECK_ERR(store.observe(no_source), ErrorCode::InvalidArgument);
  no_source.key.source = SourceId::from_u64(1);
  no_source.ttl = limits::kMaxEvidenceTtlNanos + 1;
  MF_CHECK_ERR(store.observe(no_source), ErrorCode::InvalidArgument);
}

MF_TEST(window, verdicts_and_close_policy) {
  Window window;
  window.id = WindowId::from_u64(1);
  window.name = "night";
  window.opens_at = 1000;
  window.closes_at = 2000;
  window.min_lead_time = 100;
  window.on_close = OnWindowClose::AbortAndRestore;
  const std::vector<Window> windows{window};

  MF_CHECK_EQ(evaluate_window(windows, {}, 500, false).verdict, WindowVerdict::NotYetOpen);
  MF_CHECK_EQ(evaluate_window(windows, {}, 1950, false).verdict, WindowVerdict::TooLateToStart);
  MF_CHECK_EQ(evaluate_window(windows, {}, 1000, false).verdict, WindowVerdict::Open);
  MF_CHECK_EQ(evaluate_window(windows, {}, 1901, false).verdict, WindowVerdict::TooLateToStart);
  MF_CHECK_EQ(evaluate_window(windows, {}, 1900, false).verdict, WindowVerdict::Open);
  MF_CHECK_EQ(evaluate_window(windows, {}, 1999, false).verdict, WindowVerdict::TooLateToStart);
  MF_CHECK_EQ(evaluate_window(windows, {}, 2000, false).verdict, WindowVerdict::Closed);
  MF_CHECK_EQ(evaluate_window({}, {}, 2000, false).verdict, WindowVerdict::NoWindowConfigured);
  MF_CHECK_EQ(evaluate_window({}, {}, 2000, true).verdict, WindowVerdict::NoMatchingWindow);
  MF_CHECK(evaluate_window({}, {}, 2000, false).allows_start());
  MF_CHECK(!evaluate_window({}, {}, 2000, true).allows_start());
  MF_CHECK(window_closed_after(window, 2000));
  MF_CHECK(!window_closed_after(window, 1999));
}

MF_TEST(window, deterministic_choice_among_overlapping_windows) {
  Window narrow;
  narrow.id = WindowId::from_u64(2);
  narrow.opens_at = 1000;
  narrow.closes_at = 5000;
  narrow.min_lead_time = 0;
  Window wide;
  wide.id = WindowId::from_u64(1);
  wide.opens_at = 900;
  wide.closes_at = 6000;
  wide.min_lead_time = 0;
  const std::vector<Window> windows{narrow, wide};
  const WindowEvaluation evaluation = evaluate_window(windows, {}, 2000, false);
  MF_CHECK_EQ(evaluation.verdict, WindowVerdict::Open);
  MF_CHECK_EQ(evaluation.window.value(), 1u);
}

MF_TEST(lifecycle, forward_chain_is_strict) {
  const JobState chain[] = {JobState::Proposed,   JobState::Validated, JobState::Prerequisites,
                            JobState::Ready,      JobState::InMaintenance, JobState::Verifying,
                            JobState::Restoring,  JobState::Complete};
  for (std::size_t i = 0; i + 2 < std::size(chain); ++i) {
    MF_CHECK(!check_transition(chain[i], chain[i + 2], false).allowed);
  }
  for (std::size_t i = 0; i + 1 < std::size(chain); ++i) {
    MF_CHECK(check_transition(chain[i], chain[i + 1], false).allowed);
  }
  for (std::size_t i = 0; i < std::size(chain); ++i) {
    MF_CHECK(!check_transition(chain[i], chain[i], false).allowed);
  }
}

MF_TEST(lifecycle, service_removal_freezes_backward_movement) {
  // Without service removal, a job may fall back to an earlier planning stage.
  MF_CHECK(check_transition(JobState::Ready, JobState::Prerequisites, false).allowed);
  MF_CHECK(check_transition(JobState::Prerequisites, JobState::Validated, false).allowed);
  MF_CHECK(!check_transition(JobState::Ready, JobState::Proposed, false).allowed);
  // Once service is removed, only forward movement toward restoration is legal.
  for (const JobState from : {JobState::InMaintenance, JobState::Verifying, JobState::Restoring}) {
    for (const JobState to : {JobState::Proposed, JobState::Validated, JobState::Prerequisites,
                              JobState::Ready}) {
      const TransitionCheck check = check_transition(from, to, true);
      MF_CHECK(!check.allowed);
    }
    MF_CHECK(!check_transition(from, JobState::Cancelled, true).allowed);
    MF_CHECK(check_transition(from, JobState::Failed, true).allowed);
  }
  MF_CHECK(check_transition(JobState::InMaintenance, JobState::Verifying, true).allowed);
  MF_CHECK(check_transition(JobState::Verifying, JobState::Restoring, true).allowed);
  MF_CHECK(check_transition(JobState::Restoring, JobState::Complete, true).allowed);
}

MF_TEST(lifecycle, blocked_resumes_only_where_safe) {
  MF_CHECK(!check_transition(JobState::Blocked, JobState::Proposed, false).allowed);
  MF_CHECK(!check_transition(JobState::Blocked, JobState::Validated, false).allowed);
  MF_CHECK(check_transition(JobState::Blocked, JobState::Prerequisites, false).allowed);
  MF_CHECK(check_transition(JobState::Blocked, JobState::Ready, false).allowed);
  MF_CHECK(!check_transition(JobState::Blocked, JobState::Verifying, false).allowed);
  MF_CHECK(!check_transition(JobState::Blocked, JobState::Prerequisites, true).allowed);
  MF_CHECK(!check_transition(JobState::Blocked, JobState::Ready, true).allowed);
  MF_CHECK(check_transition(JobState::Blocked, JobState::Verifying, true).allowed);
  MF_CHECK(check_transition(JobState::Blocked, JobState::Restoring, true).allowed);
}

MF_TEST(lifecycle, terminal_states_absorb) {
  for (const JobState terminal : {JobState::Complete, JobState::Cancelled, JobState::Failed}) {
    MF_CHECK(is_terminal(terminal));
    for (std::size_t i = 0; i < kJobStateCount; ++i) {
      MF_CHECK(!check_transition(terminal, static_cast<JobState>(i), false).allowed);
      MF_CHECK(!check_transition(terminal, static_cast<JobState>(i), true).allowed);
    }
  }
  MF_CHECK(!is_terminal(JobState::Blocked));
  MF_CHECK(!check_transition(JobState::Complete, JobState::Blocked, false).allowed);
}

MF_TEST(lifecycle, allowed_next_states_is_deterministic_and_ordered) {
  const std::vector<JobState> states = allowed_next_states(JobState::Ready, false);
  MF_CHECK(!states.empty());
  MF_CHECK_EQ(std::set<JobState>(states.begin(), states.end()).size(), states.size());
  MF_CHECK_EQ(allowed_next_states(JobState::Ready, false).size(), states.size());
}

MF_TEST(authority, reserve_renew_release_and_fencing) {
  ControllerIncarnation owner;
  owner.controller = ControllerId::from_u64(1);
  owner.boot_epoch = BootEpoch::from_u64(1);
  owner.nonce = 4;

  AuthorityLedger ledger(1000);
  AuthorityReservation reservation;
  reservation.job = JobId::from_u64(1);
  reservation.generation = GenerationId::from_u64(1);
  reservation.attempt = AttemptId{JobId::from_u64(1), GenerationId::from_u64(1),
                                  AttemptOrdinal::from_u64(1)};
  reservation.owner = owner;
  reservation.targets = {TargetId{TargetKind::Link, 1}};
  reservation.granted_at = 0;
  reservation.expires_at = 900;

  Result<AuthorityId> granted = ledger.reserve(reservation);
  MF_CHECK_OK(granted);
  MF_CHECK(ledger.find_for_job(JobId::from_u64(1), GenerationId::from_u64(1)) != nullptr);
  MF_CHECK_ERR(ledger.reserve(reservation), ErrorCode::AlreadyExists);

  // A different incarnation cannot renew or release what it did not grant.
  ControllerIncarnation intruder = owner;
  intruder.nonce = 99;
  MF_CHECK_ERR(ledger.renew(granted.value(), reservation.attempt, intruder, 100, 0),
               ErrorCode::StaleIncarnation);
  MF_CHECK_ERR(ledger.release(granted.value(), intruder), ErrorCode::StaleIncarnation);

  // A stale attempt on the same incarnation is refused too.
  AttemptId other_attempt = reservation.attempt;
  other_attempt.ordinal = AttemptOrdinal::from_u64(2);
  MF_CHECK_ERR(ledger.renew(granted.value(), other_attempt, owner, 100, 0),
               ErrorCode::StaleAttempt);

  MF_CHECK_OK(ledger.renew(granted.value(), reservation.attempt, owner, 100, 0));
  MF_CHECK_EQ(ledger.find(granted.value())->expires_at, 1000);
  MF_CHECK_EQ(ledger.expire(999), 0u);
  MF_CHECK_EQ(ledger.expire(1001), 1u);
  MF_CHECK_EQ(ledger.size(), 0u);
}

MF_TEST(authority, reserve_validates_bounds) {
  AuthorityLedger ledger(1000);
  ControllerIncarnation owner;
  owner.controller = ControllerId::from_u64(1);
  owner.boot_epoch = BootEpoch::from_u64(1);
  owner.nonce = 1;
  AuthorityReservation reservation;
  reservation.job = JobId::from_u64(1);
  reservation.generation = GenerationId::from_u64(1);
  reservation.attempt = AttemptId{JobId::from_u64(1), GenerationId::from_u64(1),
                                  AttemptOrdinal::from_u64(1)};
  reservation.owner = owner;

  AuthorityReservation no_targets = reservation;
  no_targets.targets.clear();
  no_targets.expires_at = 10;
  MF_CHECK_ERR(ledger.reserve(no_targets), ErrorCode::InvalidArgument);

  AuthorityReservation negative = reservation;
  negative.targets = {TargetId{TargetKind::Link, 1}};
  negative.granted_at = 100;
  negative.expires_at = 50;
  MF_CHECK_ERR(ledger.reserve(negative), ErrorCode::InvalidArgument);

  AuthorityReservation too_long = reservation;
  too_long.targets = {TargetId{TargetKind::Link, 1}};
  too_long.expires_at = limits::kMaxAuthorityLeaseNanos + 1;
  MF_CHECK_ERR(ledger.reserve(too_long), ErrorCode::InvalidArgument);

  AuthorityReservation without_owner = reservation;
  without_owner.targets = {TargetId{TargetKind::Link, 1}};
  without_owner.owner = ControllerIncarnation{};
  without_owner.expires_at = 100;
  MF_CHECK_ERR(ledger.reserve(without_owner), ErrorCode::InvalidArgument);
}

MF_TEST(authority, restart_revokes_previous_incarnations) {
  ControllerIncarnation first;
  first.controller = ControllerId::from_u64(1);
  first.boot_epoch = BootEpoch::from_u64(1);
  first.nonce = 11;
  ControllerIncarnation second = first;
  second.boot_epoch = BootEpoch::from_u64(2);
  second.nonce = 22;

  AuthorityLedger ledger(1000);
  AuthorityReservation reservation;
  reservation.job = JobId::from_u64(1);
  reservation.generation = GenerationId::from_u64(1);
  reservation.attempt = AttemptId{JobId::from_u64(1), GenerationId::from_u64(1),
                                  AttemptOrdinal::from_u64(1)};
  reservation.owner = first;
  reservation.targets = {TargetId{TargetKind::Link, 1}};
  reservation.expires_at = 100000;
  MF_CHECK_OK(ledger.reserve(reservation));
  MF_CHECK_EQ(ledger.size(), 1u);
  const std::vector<AuthorityReservation> revoked = ledger.fence_older_incarnations(second);
  MF_CHECK_EQ(revoked.size(), 1u);
  MF_CHECK_EQ(ledger.size(), 0u);
  MF_CHECK_EQ(ledger.fence_older_incarnations(second).size(), 0u);
}

MF_TEST_MAIN()
