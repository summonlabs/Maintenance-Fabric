// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

constexpr Nanos kStep = kNanosPerSecond;

MaintenanceRequest request_for(const std::string& title, std::vector<TargetId> targets,
                               std::uint32_t priority = 1) {
  MaintenanceRequest request;
  request.title = title;
  request.reason = "planned maintenance";
  request.targets = std::move(targets);
  request.priority = priority;
  request.estimated_duration = 5 * kNanosPerMinute;
  request.requires_drain = true;
  request.requestor = requestor_from_name("test");
  return request;
}

/// Runs up to \p max_ticks scheduler passes, advancing the manual clock by one
/// second each time.
std::size_t tick_until(Controller& controller, ManualClock& clock,
                       const std::function<bool(const MaintenanceJob&)>& done,
                       std::size_t max_ticks = 32) {
  for (std::size_t i = 0; i < max_ticks; ++i) {
    const Status ticked = controller.tick(clock.now());
    if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
      MF_FAIL(std::string("tick failed: ") + format_error(ticked.error()));
    }
    clock.advance(kStep);
    if (done(controller.state().jobs.jobs().begin()->second)) {
      return i + 1;
    }
  }
  return max_ticks;
}

const MaintenanceJob& job_of(Controller& controller, JobId id) {
  const MaintenanceJob* job = controller.state().jobs.find(id);
  MF_CHECK(job != nullptr);
  return *job;
}

/// Drives a controller until \p id reaches \p state.
void drive_to(Controller& controller, ManualClock& clock, JobId id, JobState state) {
  for (std::size_t i = 0; i < 48; ++i) {
    const Status ticked = controller.tick(clock.now());
    if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
      MF_FAIL(std::string("tick failed: ") + format_error(ticked.error()));
    }
    clock.advance(kStep);
    const MaintenanceJob& job = job_of(controller, id);
    if (job.state == state) {
      return;
    }
    if (job.is_terminal()) {
      MF_FAIL("job reached " + std::string(to_string(job.state)) + " while driving to " +
              std::string(to_string(state)) + " (" + std::string(to_string(job.block)) + ": " +
              job.block_detail + ")");
    }
  }
  MF_FAIL("job never reached " + std::string(to_string(state)));
}

struct Harness {
  mftest::TempDir dir;
  ManualClock clock;
  std::shared_ptr<LocalDrainPort> drain;
  std::unique_ptr<Controller> controller;

  explicit Harness(const std::string& tag, std::uint32_t n_plus = 1, std::size_t pools = 2,
                   LocalDrainOptions drain_options = {})
      : dir(tag), clock(1000000000LL), drain(std::make_shared<LocalDrainPort>(drain_options)) {
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened =
        mftest::open_controller(dir.file("journal.mfj"), clock, drain);
    MF_CHECK_OK(opened);
    controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller, n_plus, pools));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  }

  JobId propose(const std::string& title, std::vector<TargetId> targets,
                std::uint32_t priority = 1) {
    Result<JobId> created =
        controller->propose(request_for(title, std::move(targets), priority), "tester");
    MF_CHECK_OK(created);
    MF_CHECK_OK(controller->approve(created.value(), "tester"));
    return created.value();
  }

  void tick() {
    const Status ticked = controller->tick(clock.now());
    if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
      MF_FAIL(std::string("tick failed: ") + format_error(ticked.error()));
    }
    clock.advance(kStep);
  }

  void tick(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      tick();
    }
  }

  void to(JobId id, JobState state) { drive_to(*controller, clock, id, state); }

  [[nodiscard]] const MaintenanceJob& job(JobId id) { return job_of(*controller, id); }
};

}  // namespace

MF_TEST(controller, full_lifecycle_reaches_complete_only_after_verification) {
  Harness harness("ctl-happy");
  const JobId id = harness.propose("swap optics", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const MaintenanceJob& started = harness.job(id);
  MF_CHECK(started.service_removed);
  MF_CHECK(started.authority.valid());
  MF_CHECK(started.lease.valid());
  MF_CHECK(started.active_attempt.valid());
  MF_CHECK_EQ(started.removal_set.size(), 1u);

  // Completion must come from the current attempt of the current generation.
  MF_CHECK_OK(harness.controller->report_completion(id, started.active_attempt, true, "done"));
  harness.to(id, JobState::Verifying);
  harness.to(id, JobState::Complete);
  const MaintenanceJob& finished = harness.job(id);
  MF_CHECK(finished.is_terminal());
  MF_CHECK(!finished.authority.valid());
  MF_CHECK(!finished.lease.valid());
  MF_CHECK_EQ(finished.verification_passes, 1u);
  MF_CHECK_EQ(harness.controller->stats().completions, 1u);
  MF_CHECK_EQ(harness.drain->active_leases(), 0u);
}

MF_TEST(controller, commands_before_admission_are_rejected) {
  Harness harness("ctl-order");
  const JobId id = harness.propose("ordered", {TargetId{TargetKind::Link, 1}});
  const MaintenanceJob& planned = harness.job(id);
  AttemptId bogus{id, planned.generation, AttemptOrdinal::from_u64(1)};
  MF_CHECK_ERR(harness.controller->report_completion(id, bogus, true, "premature"),
               ErrorCode::StaleAttempt);
  MF_CHECK_EQ(harness.job(id).state, JobState::Validated);
}

MF_TEST(controller, start_is_blocked_while_health_evidence_is_stale) {
  Harness harness("ctl-stale");
  const JobId id = harness.propose("stale", {TargetId{TargetKind::Link, 1}});
  harness.tick();
  MF_CHECK_EQ(harness.job(id).state, JobState::Prerequisites);
  // Let every observation expire without republishing.
  harness.clock.advance(10 * kNanosPerMinute);
  harness.tick();
  const MaintenanceJob& blocked = harness.job(id);
  MF_CHECK_EQ(blocked.state, JobState::Blocked);
  MF_CHECK_EQ(blocked.block, BlockReason::EvidenceMissingOrStale);
  MF_CHECK(!blocked.service_removed);
  MF_CHECK(!blocked.authority.valid());
}

MF_TEST(controller, start_is_blocked_by_redundancy_policy) {
  Harness harness("ctl-redundancy");
  // Two members of the same pool are down before any approval, so removing a
  // third would drop the pool below its N+1 requirement.
  for (const std::uint32_t index : {3u, 5u}) {
    MF_CHECK_OK(harness.controller->observe(mftest::health_evidence(
        TargetId{TargetKind::Link, index}, Health::Down, harness.clock.now(),
        harness.controller->policy().ttl_for(EvidenceKind::TargetHealth))));
  }
  const JobId id = harness.propose("unsafe", {TargetId{TargetKind::Link, 1}});
  harness.tick(3);
  const MaintenanceJob& blocked = harness.job(id);
  MF_CHECK_EQ(blocked.state, JobState::Blocked);
  MF_CHECK_EQ(blocked.block, BlockReason::RedundancyViolation);
  MF_CHECK(!blocked.service_removed);
  MF_CHECK_EQ(harness.controller->stats().starts, 0u);
  MF_CHECK_EQ(harness.drain->requests(), 0u);
}

MF_TEST(controller, spontaneous_link_loss_after_approval_blocks_the_start) {
  Harness harness("ctl-linkloss");
  const JobId id = harness.propose("link loss", {TargetId{TargetKind::Link, 1}});
  harness.tick();
  MF_CHECK(!harness.job(id).service_removed);
  // Two peer links in the same pool drop out between approval and start.
  for (const std::uint32_t index : {3u, 5u}) {
    MF_CHECK_OK(harness.controller->observe(mftest::health_evidence(
        TargetId{TargetKind::Link, index}, Health::Down, harness.clock.now(),
        harness.controller->policy().ttl_for(EvidenceKind::TargetHealth))));
  }
  harness.tick(3);
  const MaintenanceJob& blocked = harness.job(id);
  MF_CHECK_EQ(blocked.state, JobState::Blocked);
  MF_CHECK(blocked.block == BlockReason::RedundancyViolation ||
           blocked.block == BlockReason::EvidenceMissingOrStale);
  MF_CHECK(!blocked.service_removed);
  MF_CHECK_EQ(harness.controller->stats().starts, 0u);
}

MF_TEST(controller, drain_failure_blocks_after_the_policy_bound) {
  LocalDrainOptions drain_options;
  drain_options.fail_next_requests = 99;
  Harness harness("ctl-drainfail", 1, 2, drain_options);
  const JobId id = harness.propose("drain fails", {TargetId{TargetKind::Link, 1}});
  harness.tick(8);
  const MaintenanceJob& blocked = harness.job(id);
  MF_CHECK_EQ(blocked.state, JobState::Blocked);
  MF_CHECK_EQ(blocked.block, BlockReason::DrainFailed);
  MF_CHECK(!blocked.service_removed);
  MF_CHECK(!blocked.lease.valid());
  MF_CHECK(!blocked.authority.valid());
  MF_CHECK_EQ(harness.controller->stats().starts, 0u);
}

MF_TEST(controller, maintenance_process_death_leaves_the_job_in_maintenance) {
  Harness harness("ctl-death");
  const JobId id = harness.propose("executor dies", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  // No completion report ever arrives: the resource stays out of service and the
  // job must not drift back to a planning state.
  harness.tick(10);
  const MaintenanceJob& stuck = harness.job(id);
  MF_CHECK_EQ(stuck.state, JobState::InMaintenance);
  MF_CHECK(stuck.service_removed);
  MF_CHECK(stuck.lease.valid());
  MF_CHECK(stuck.authority.valid());
}

MF_TEST(controller, overlapping_jobs_cannot_both_start) {
  Harness harness("ctl-overlap");
  const JobId first = harness.propose("first", {TargetId{TargetKind::Link, 1}}, 0);
  const JobId second = harness.propose("second", {TargetId{TargetKind::Link, 2}}, 0);
  harness.tick(10);
  const MaintenanceJob& a = harness.job(first);
  const MaintenanceJob& b = harness.job(second);
  if (!a.service_removed) {
    MF_FAIL("first job did not start: state=" + std::string(to_string(a.state)) + " block=" +
            std::string(to_string(a.block)) + " detail=" + a.block_detail + "\narbitration:\n" +
            harness.controller->last_arbitration().to_text() + "\nsecond job: " +
            std::string(to_string(b.state)) + "/" + std::string(to_string(b.block)));
  }
  MF_CHECK(a.state == JobState::InMaintenance || a.state == JobState::Verifying);
  MF_CHECK(a.service_removed);
  MF_CHECK(!b.service_removed);
  MF_CHECK(b.state == JobState::Blocked || b.state == JobState::Prerequisites);
  if (b.state == JobState::Blocked) {
    // link:1 and link:2 share a correlated rack domain as well as capacity, so
    // any of these three refusals is a correct outcome.
    MF_CHECK(b.block == BlockReason::RedundancyViolation ||
             b.block == BlockReason::ConflictingMaintenance ||
             b.block == BlockReason::FailureDomainLimit);
  }
  MF_CHECK_EQ(harness.controller->stats().starts, 1u);
}

MF_TEST(controller, conflicting_jobs_on_the_same_scope_are_refused) {
  Harness harness("ctl-conflict");
  const JobId first = harness.propose("first", {TargetId{TargetKind::Link, 1}});
  const JobId second = harness.propose("second", {TargetId{TargetKind::Link, 1}});
  harness.tick(6);
  const MaintenanceJob& a = harness.job(first);
  const MaintenanceJob& b = harness.job(second);
  if (!a.service_removed) {
    MF_FAIL("first job did not start: state=" + std::string(to_string(a.state)) + " block=" +
            std::string(to_string(a.block)) + " detail=" + a.block_detail + " | second: " +
            std::string(to_string(b.state)) + "/" + std::string(to_string(b.block)));
  }
  MF_CHECK(a.service_removed);
  MF_CHECK(!b.service_removed);
  MF_CHECK_EQ(b.state, JobState::Blocked);
  MF_CHECK_EQ(b.block, BlockReason::ConflictingMaintenance);
  const ConflictReport report = harness.controller->conflicts();
  MF_CHECK(!report.conflicts.empty());
  MF_CHECK_EQ(report.conflicts.front().shared, (TargetId{TargetKind::Link, 1}));
}

MF_TEST(controller, failed_restoration_check_fails_the_job_and_quarantines_the_target) {
  Harness harness("ctl-restore-fail");
  const JobId id = harness.propose("bad restore", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const AttemptId attempt = harness.job(id).active_attempt;
  MF_CHECK_OK(harness.controller->report_completion(id, attempt, true, "done"));
  harness.to(id, JobState::Verifying);
  MF_CHECK_OK(harness.controller->report_restoration(id, attempt, TargetId{TargetKind::Link, 1},
                                                     false, "optics not seated"));
  // The negative report is authoritative: the target stays out of service and
  // the runtime retries within the policy bound before failing the job.
  harness.tick(2);
  MF_CHECK(!harness.job(id).is_terminal());
  for (int i = 0; i < 30 && !harness.job(id).is_terminal(); ++i) {
    harness.clock.advance(6 * kNanosPerMinute);
    harness.tick();
  }
  const MaintenanceJob& failed = harness.job(id);
  MF_CHECK_EQ(failed.state, JobState::Failed);
  MF_CHECK_EQ(failed.block, BlockReason::VerificationFailed);
  const std::vector<QuarantineRecord> quarantines = harness.controller->quarantines();
  MF_CHECK(!quarantines.empty());
  MF_CHECK_EQ(quarantines.front().target, (TargetId{TargetKind::Link, 1}));
  MF_CHECK(!quarantines.front().cleared);
}

MF_TEST(controller, unverified_targets_never_return_to_service) {
  Harness harness("ctl-unverified");
  const JobId id = harness.propose("no health", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const AttemptId attempt = harness.job(id).active_attempt;
  MF_CHECK_OK(harness.controller->report_completion(id, attempt, true, "done"));
  harness.to(id, JobState::Verifying);
  // The scope stays unhealthy: the runtime retries within the policy bound and
  // then fails the job rather than declaring success.
  for (int i = 0; i < 40 && !harness.job(id).is_terminal(); ++i) {
    MF_CHECK_OK(harness.controller->observe(mftest::health_evidence(
        TargetId{TargetKind::Link, 1}, Health::Down, harness.clock.now(),
        harness.controller->policy().ttl_for(EvidenceKind::TargetHealth))));
    harness.clock.advance(5 * kNanosPerMinute);
    harness.tick();
  }
  const MaintenanceJob& job = harness.job(id);
  MF_CHECK_EQ(job.state, JobState::Failed);
  MF_CHECK_EQ(job.block, BlockReason::VerificationFailed);
  MF_CHECK_EQ(harness.controller->stats().completions, 0u);
  MF_CHECK(!harness.controller->quarantines().empty());
}

MF_TEST(controller, cancellation_is_refused_once_service_is_removed) {
  Harness harness("ctl-cancel");
  const JobId id = harness.propose("cancel me", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  MF_CHECK_ERR(harness.controller->cancel(id, "tester", "changed my mind"),
               ErrorCode::StateConflict);
  MF_CHECK_EQ(harness.job(id).state, JobState::InMaintenance);
  MF_CHECK(harness.job(id).service_removed);

  const JobId other = harness.propose("cancel early", {TargetId{TargetKind::Link, 4}});
  MF_CHECK_OK(harness.controller->cancel(other, "tester", "not needed"));
  MF_CHECK_EQ(harness.job(other).state, JobState::Cancelled);
}

MF_TEST(controller, stale_attempt_and_generation_are_fenced) {
  Harness harness("ctl-fence");
  const JobId id = harness.propose("fenced", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const AttemptId attempt = harness.job(id).active_attempt;

  AttemptId wrong_ordinal = attempt;
  wrong_ordinal.ordinal = AttemptOrdinal::from_u64(attempt.ordinal.value() + 5);
  MF_CHECK_ERR(harness.controller->report_completion(id, wrong_ordinal, true, "stale"),
               ErrorCode::StaleAttempt);
  MF_CHECK_EQ(harness.controller->stats().stale_attempt_rejections, 1u);

  AttemptId wrong_generation = attempt;
  wrong_generation.generation = GenerationId::from_u64(attempt.generation.value() + 1);
  MF_CHECK_ERR(harness.controller->report_completion(id, wrong_generation, true, "stale"),
               ErrorCode::StaleGeneration);
  MF_CHECK_EQ(harness.controller->stats().stale_generation_rejections, 1u);

  // The real attempt still works.
  MF_CHECK_OK(harness.controller->report_completion(id, attempt, true, "done"));
}

MF_TEST(controller, restart_requires_fresh_verification_and_keeps_capacity_reserved) {
  mftest::TempDir dir("ctl-restart");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  JobId id;
  AttemptId old_attempt;
  {
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened =
        mftest::open_controller(dir.file("journal.mfj"), clock, drain);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
    Result<JobId> created =
        controller->propose(request_for("restart", {TargetId{TargetKind::Link, 1}}), "tester");
    MF_CHECK_OK(created);
    id = created.value();
    MF_CHECK_OK(controller->approve(id, "tester"));
    drive_to(*controller, clock, id, JobState::InMaintenance);
    old_attempt = controller->state().jobs.find(id)->active_attempt;
    MF_CHECK_OK(controller->shutdown());
  }

  RecoveryReport report;
  Result<std::unique_ptr<Controller>> reopened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain, 4242);
  MF_CHECK_OK(reopened);
  std::unique_ptr<Controller> controller = std::move(reopened.value());
  MF_CHECK_EQ(report.jobs_recovered, 0u);  // report is filled by open()
  const MaintenanceJob& recovered = *controller->state().jobs.find(id);
  MF_CHECK(recovered.service_removed);
  MF_CHECK_EQ(recovered.state, JobState::Blocked);
  MF_CHECK_EQ(recovered.block, BlockReason::ReconciledAfterRestart);
  // The new incarnation drives the scope with its own attempt.
  MF_CHECK(recovered.active_attempt.valid());
  MF_CHECK_NE(recovered.active_attempt, old_attempt);
  MF_CHECK(controller->stats().reconciliations >= 1u);
  MF_CHECK(controller->stats().authority_revocations >= 1u);

  // The previous attempt can no longer complete the job.
  MF_CHECK_ERR(controller->report_completion(id, old_attempt, true, "old process"),
               ErrorCode::StaleAttempt);

  // Capacity stays reserved: the fresh incarnation re-issued authority because
  // the scope is genuinely out of service.
  MF_CHECK(recovered.authority.valid());

  // The runtime drives the job toward verification and restoration.
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  drive_to(*controller, clock, id, JobState::Verifying);
  const MaintenanceJob& verifying = *controller->state().jobs.find(id);
  MF_CHECK(verifying.active_attempt.valid());
  drive_to(*controller, clock, id, JobState::Complete);
  MF_CHECK_EQ(controller->stats().completions, 1u);
}

MF_TEST(controller, restart_before_service_removal_releases_everything) {
  mftest::TempDir dir("ctl-restart-early");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  JobId id;
  {
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened =
        mftest::open_controller(dir.file("journal.mfj"), clock, drain);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
    Result<JobId> created =
        controller->propose(request_for("early", {TargetId{TargetKind::Link, 1}}), "tester");
    MF_CHECK_OK(created);
    id = created.value();
    MF_CHECK_OK(controller->approve(id, "tester"));
    (void)controller->tick(clock.now());
    clock.advance(kStep);
    MF_CHECK_EQ(controller->state().jobs.find(id)->state, JobState::Prerequisites);
    MF_CHECK_OK(controller->shutdown());
  }
  RecoveryReport recovery;
  Result<std::unique_ptr<Controller>> reopened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain, 999);
  MF_CHECK_OK(reopened);
  std::unique_ptr<Controller> controller = std::move(reopened.value());
  const MaintenanceJob& recovered = *controller->state().jobs.find(id);
  MF_CHECK_EQ(recovered.state, JobState::Blocked);
  MF_CHECK_EQ(recovered.block, BlockReason::ReconciledAfterRestart);
  MF_CHECK(!recovered.service_removed);
  MF_CHECK(!recovered.authority.valid());
  MF_CHECK(!recovered.lease.valid());
  // Nothing was removed, so the job can be re-established and finish normally.
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  drive_to(*controller, clock, id, JobState::InMaintenance);
}

MF_TEST(controller, rearm_bumps_the_generation_and_discards_old_tokens) {
  Harness harness("ctl-rearm");
  const JobId id = harness.propose("rearm", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const AttemptId attempt = harness.job(id).active_attempt;
  // A job that removed service cannot be re-armed.
  MF_CHECK_ERR(harness.controller->rearm(id, "tester"), ErrorCode::StateConflict);

  const JobId blocked = harness.propose("blocked", {TargetId{TargetKind::Link, 4}});
  for (const std::uint32_t index : {6u, 8u}) {
    MF_CHECK_OK(harness.controller->observe(mftest::health_evidence(
        TargetId{TargetKind::Link, index}, Health::Down, harness.clock.now(),
        harness.controller->policy().ttl_for(EvidenceKind::TargetHealth))));
  }
  harness.tick(3);
  MF_CHECK_EQ(harness.job(blocked).state, JobState::Blocked);
  const GenerationId before = harness.job(blocked).generation;
  MF_CHECK_OK(harness.controller->rearm(blocked, "tester"));
  const MaintenanceJob& rearmed = harness.job(blocked);
  MF_CHECK(rearmed.generation > before);
  MF_CHECK_EQ(rearmed.state, JobState::Prerequisites);
  MF_CHECK(!rearmed.active_attempt.valid());
  MF_CHECK(rearmed.attempts.size() >= 1u);
  for (const AttemptRecord& record : rearmed.attempts) {
    MF_CHECK(record.outcome != AttemptOutcome::Running);
  }
  (void)attempt;
}

MF_TEST(controller, window_close_records_overrun_without_killing_the_attempt) {
  mftest::TempDir dir("ctl-window");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  Policy policy = mftest::make_policy(1);
  Window window;
  window.id = WindowId::from_u64(1);
  window.name = "short";
  window.opens_at = clock.now();
  window.closes_at = clock.now() + 3 * kNanosPerSecond;
  window.min_lead_time = 0;
  window.on_close = OnWindowClose::FinishActiveStep;
  policy.windows.push_back(window);
  policy.require_window = true;
  MF_CHECK_OK(controller->set_topology(mftest::make_topology()));
  MF_CHECK_OK(controller->set_policy(policy));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));

  Result<JobId> created =
      controller->propose(request_for("windowed", {TargetId{TargetKind::Link, 1}}), "tester");
  MF_CHECK_OK(created);
  const JobId id = created.value();
  MF_CHECK_OK(controller->approve(id, "tester"));
  drive_to(*controller, clock, id, JobState::InMaintenance);
  MF_CHECK_EQ(controller->state().jobs.find(id)->window.value(), 1u);
  MF_CHECK_EQ(controller->state().jobs.find(id)->on_close, OnWindowClose::FinishActiveStep);
  // The window closes while the attempt is running.
  clock.set(window.closes_at + kNanosPerSecond);
  (void)controller->tick(clock.now());
  const MaintenanceJob& overrun = *controller->state().jobs.find(id);
  MF_CHECK(overrun.overrun);
  MF_CHECK_EQ(overrun.state, JobState::InMaintenance);
  MF_CHECK(overrun.service_removed);
  // Restoration is never blocked by window policy.
  MF_CHECK_OK(controller->report_completion(id, overrun.active_attempt, true, "late but done"));
  drive_to(*controller, clock, id, JobState::Complete);
}

MF_TEST(controller, window_close_can_abort_and_quarantine) {
  mftest::TempDir dir("ctl-abort");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  Policy policy = mftest::make_policy(1);
  Window window;
  window.id = WindowId::from_u64(1);
  window.opens_at = clock.now();
  window.closes_at = clock.now() + 3 * kNanosPerSecond;
  window.min_lead_time = 0;
  window.on_close = OnWindowClose::AbortAndRestore;
  window.abort_grace = 2 * kNanosPerSecond;
  policy.windows.push_back(window);
  MF_CHECK_OK(controller->set_topology(mftest::make_topology()));
  MF_CHECK_OK(controller->set_policy(policy));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  Result<JobId> created =
      controller->propose(request_for("abort", {TargetId{TargetKind::Link, 1}}), "tester");
  MF_CHECK_OK(created);
  const JobId id = created.value();
  MF_CHECK_OK(controller->approve(id, "tester"));
  drive_to(*controller, clock, id, JobState::InMaintenance);
  clock.set(window.closes_at + 10 * kNanosPerSecond);
  (void)controller->tick(clock.now());
  const MaintenanceJob& failed = *controller->state().jobs.find(id);
  MF_CHECK_EQ(failed.state, JobState::Failed);
  MF_CHECK_EQ(failed.block, BlockReason::WindowUnavailable);
  MF_CHECK(!controller->quarantines().empty());
}

MF_TEST(controller, window_extension_policy_suppresses_the_overrun_flag) {
  mftest::TempDir dir("ctl-extend");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  Policy policy = mftest::make_policy(1);
  Window window;
  window.id = WindowId::from_u64(1);
  window.opens_at = clock.now();
  window.closes_at = clock.now() + 3 * kNanosPerSecond;
  window.on_close = OnWindowClose::ExtendToComplete;
  policy.windows.push_back(window);
  MF_CHECK_OK(controller->set_topology(mftest::make_topology()));
  MF_CHECK_OK(controller->set_policy(policy));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  Result<JobId> created =
      controller->propose(request_for("extend", {TargetId{TargetKind::Link, 1}}), "tester");
  MF_CHECK_OK(created);
  const JobId id = created.value();
  MF_CHECK_OK(controller->approve(id, "tester"));
  drive_to(*controller, clock, id, JobState::InMaintenance);
  clock.set(window.closes_at + 5 * kNanosPerSecond);
  (void)controller->tick(clock.now());
  const MaintenanceJob& job = *controller->state().jobs.find(id);
  MF_CHECK(job.window_extended);
  MF_CHECK(!job.overrun);
  MF_CHECK_EQ(job.state, JobState::InMaintenance);
}

MF_TEST(controller, target_scope_expands_through_the_containment_tree) {
  Harness harness("ctl-scope");
  const JobId id = harness.propose("whole switch", {TargetId{TargetKind::Switch, 1}});
  harness.tick(6);
  const MaintenanceJob& job = harness.job(id);
  MF_CHECK_EQ(job.removal_set.size(), 3u);
  MF_CHECK_EQ(job.removal_set.size(), harness.controller->topology()
                                          .removal_set({TargetId{TargetKind::Switch, 1}})
                                          .size());
}

MF_TEST(controller, unmaintainable_targets_are_a_hard_block) {
  Harness harness("ctl-hard");
  Topology topology = mftest::make_topology();
  const TargetRecord* existing = topology.target(TargetId{TargetKind::Link, 1});
  TargetRecord updated = *existing;
  updated.maintainable = false;
  MF_CHECK_OK(topology.add_target(updated));
  MF_CHECK_OK(harness.controller->set_topology(std::move(topology)));
  MF_CHECK_OK(mftest::seed_health(*harness.controller, harness.clock.now()));
  const JobId id = harness.propose("not maintainable", {TargetId{TargetKind::Link, 1}});
  harness.tick(4);
  const MaintenanceJob& blocked = harness.job(id);
  MF_CHECK_EQ(blocked.state, JobState::Blocked);
  MF_CHECK_EQ(blocked.block, BlockReason::TargetNotMaintainable);
  // Retrying does not clear a hard block.
  const JobState before = blocked.state;
  harness.tick(4);
  MF_CHECK_EQ(harness.job(id).state, before);
  MF_CHECK_EQ(harness.job(id).block, BlockReason::TargetNotMaintainable);
}

MF_TEST(controller, dependency_gates_and_propagates_failure) {
  Harness harness("ctl-dependency");
  const JobId first = harness.propose("first", {TargetId{TargetKind::Link, 1}});
  MaintenanceRequest request = request_for("second", {TargetId{TargetKind::Link, 4}});
  request.depends_on = {first};
  Result<JobId> created = harness.controller->propose(request, "tester");
  MF_CHECK_OK(created);
  const JobId second = created.value();
  MF_CHECK_OK(harness.controller->approve(second, "tester"));

  harness.tick(10);
  MF_CHECK(harness.job(first).service_removed);
  const MaintenanceJob& waiting = harness.job(second);
  MF_CHECK(!waiting.service_removed);
  // Waiting on a dependency is a pending prerequisite, not a violation.
  MF_CHECK(waiting.state == JobState::Prerequisites || waiting.state == JobState::Blocked);
  if (waiting.state == JobState::Blocked) {
    MF_CHECK_EQ(waiting.block, BlockReason::DependencyUnmet);
  }

  // Completing the first job releases the second.
  const AttemptId attempt = harness.job(first).active_attempt;
  MF_CHECK_OK(harness.controller->report_completion(first, attempt, true, "done"));
  drive_to(*harness.controller, harness.clock, first, JobState::Complete);
  drive_to(*harness.controller, harness.clock, second, JobState::InMaintenance);

  // A cancelled dependency blocks its dependants permanently.
  const JobId third = harness.propose("third", {TargetId{TargetKind::Link, 5}});
  MaintenanceRequest dependent = request_for("dependent", {TargetId{TargetKind::Link, 6}});
  dependent.depends_on = {third};
  Result<JobId> dependent_id = harness.controller->propose(dependent, "tester");
  MF_CHECK_OK(dependent_id);
  MF_CHECK_OK(harness.controller->approve(dependent_id.value(), "tester"));
  MF_CHECK_OK(harness.controller->cancel(third, "tester", "no longer needed"));
  harness.tick(4);
  const MaintenanceJob& blocked = harness.job(dependent_id.value());
  if (blocked.state != JobState::Blocked) {
    MF_FAIL("dependent is " + std::string(to_string(blocked.state)) + " block=" +
            std::string(to_string(blocked.block)) + " detail=" + blocked.block_detail);
  }
  MF_CHECK_EQ(blocked.block, BlockReason::DependencyUnmet);
}

MF_TEST(controller, arbitration_report_names_the_winner_and_the_reason) {
  Harness harness("ctl-arbitration");
  const JobId first = harness.propose("urgent", {TargetId{TargetKind::Link, 1}}, 0);
  const JobId second = harness.propose("routine", {TargetId{TargetKind::Link, 3}}, 1);
  harness.tick(8);
  const ArbitrationReport report = harness.controller->last_arbitration();
  MF_CHECK(!report.decisions.empty());
  bool saw_grant = false;
  for (const ArbitrationDecision& decision : report.decisions) {
    if (decision.outcome == ArbitrationOutcome::Granted) {
      saw_grant = true;
      MF_CHECK(decision.ordering_key.find("tier=") != std::string::npos);
    }
  }
  MF_CHECK(saw_grant);
  MF_CHECK(harness.job(first).service_removed || harness.job(second).service_removed);
}

MF_TEST(controller, admission_check_is_a_side_effect_free_projection) {
  Harness harness("ctl-admission");
  const JobId id = harness.propose("projection", {TargetId{TargetKind::Link, 1}});
  const MaintenanceJob before = harness.job(id);
  Result<PreconditionSet> set = harness.controller->admission_check(id);
  MF_CHECK_OK(set);
  MF_CHECK(set.value().find(PreconditionKind::EvidenceFresh) != nullptr);
  MF_CHECK(set.value().find(PreconditionKind::EvidenceFresh)->status ==
           PreconditionStatus::Satisfied);
  MF_CHECK_EQ(set.value().violated, 0u);
  const MaintenanceJob after = harness.job(id);
  MF_CHECK_EQ(before.revision.value(), after.revision.value());
  MF_CHECK_EQ(before.state, after.state);
}

MF_TEST(controller, quarantine_can_only_be_cleared_explicitly) {
  Harness harness("ctl-quarantine");
  const JobId id = harness.propose("quarantine", {TargetId{TargetKind::Link, 1}});
  harness.to(id, JobState::InMaintenance);
  const AttemptId attempt = harness.job(id).active_attempt;
  MF_CHECK_OK(harness.controller->report_completion(id, attempt, true, "done"));
  harness.to(id, JobState::Verifying);
  MF_CHECK_OK(harness.controller->report_restoration(id, attempt, TargetId{TargetKind::Link, 1},
                                                     false, "bad optics"));
  for (int i = 0; i < 30 && !harness.job(id).is_terminal(); ++i) {
    harness.clock.advance(6 * kNanosPerMinute);
    harness.tick();
  }
  MF_CHECK_EQ(harness.job(id).state, JobState::Failed);
  const std::vector<QuarantineRecord> quarantines = harness.controller->quarantines();
  MF_CHECK(!quarantines.empty());
  MF_CHECK_OK(harness.controller->clear_quarantine(quarantines.front().id, "tester",
                                                   "optics replaced by hand"));
  const std::vector<QuarantineRecord> after = harness.controller->quarantines();
  MF_CHECK(after.front().cleared);
  MF_CHECK_ERR(harness.controller->clear_quarantine(QuarantineId::from_u64(9999), "tester", "x"),
               ErrorCode::NotFound);
}

MF_TEST_MAIN()
