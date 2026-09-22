// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/runtime/controller.hpp"

#include <algorithm>
#include <utility>

#include "mf/core/hash.hpp"
#include "mf/engine/arbitrator.hpp"
#include "mf/engine/scheduler.hpp"

namespace mf {
namespace {

constexpr int kMaxPassesPerTick = 8;
constexpr Nanos kAbortGraceFallbackNanos = 60LL * kNanosPerSecond;
/// Minimum time a soft-blocked job stays blocked before the scheduler retries
/// it, so a persistently failing dependency does not churn the lifecycle.
constexpr Nanos kSoftBlockDwellNanos = kNanosPerSecond;

bool contains(const std::vector<TargetId>& haystack, TargetId needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

bool is_hard_block(BlockReason reason) noexcept {
  switch (reason) {
    case BlockReason::TargetUnknown:
    case BlockReason::TargetNotMaintainable:
    case BlockReason::PolicyDenied:
    case BlockReason::OperatorHold:
      return true;
    default:
      return false;
  }
}

Controller::Controller(std::unique_ptr<Store> store, DrainPortPtr drain, const Clock& clock,
                       ControllerOptions options)
    : store_(std::move(store)),
      drain_(std::move(drain)),
      clock_(&clock),
      options_(std::move(options)) {}

Controller::~Controller() = default;

Result<std::unique_ptr<Controller>> Controller::open(const ControllerOptions& options,
                                                     DrainPortPtr drain, const Clock& clock,
                                                     RecoveryReport& report) {
  if (drain == nullptr) {
    return invalid_argument("controller requires a drain port");
  }
  Result<std::unique_ptr<Store>> opened = Store::open(options.store, report);
  if (!opened.ok()) {
    return opened.error();
  }
  std::unique_ptr<Controller> controller(
      new Controller(std::move(opened.value()), std::move(drain), clock, options));

  Result<ControllerIncarnation> incarnation =
      controller->store_->begin_incarnation(options.controller_id, options.nonce);
  if (!incarnation.ok()) {
    return incarnation.error();
  }
  controller->incarnation_ = incarnation.value();
  controller->policy_ = controller->store_->state().policy;
  controller->topology_ = controller->store_->state().topology;

  const Status reconciled = controller->reconcile_locked(clock.now());
  if (!reconciled.ok()) {
    return reconciled.error();
  }
  report = controller->store_->recovery();
  return Result<std::unique_ptr<Controller>>(std::move(controller));
}

Status Controller::set_topology(Topology topology) {
  const Status valid = topology.validate();
  if (!valid.ok()) {
    return valid;
  }
  std::lock_guard<std::mutex> guard(mu_);
  const Status committed = store_->commit_topology(std::move(topology));
  if (!committed.ok()) {
    return committed;
  }
  topology_ = store_->state().topology;
  return ok_status();
}

Status Controller::set_policy(Policy policy) {
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  std::lock_guard<std::mutex> guard(mu_);
  const Status committed = store_->commit_policy(std::move(policy));
  if (!committed.ok()) {
    return committed;
  }
  policy_ = store_->state().policy;
  return ok_status();
}

EvidenceRecord Controller::make_evidence_locked(EvidenceKind kind, const EvidenceSubject& subject,
                                                SourceId source, Revision revision, Nanos now,
                                                Nanos ttl, const EvidencePayload& payload,
                                                JobId job, const AttemptId& attempt) {
  EvidenceRecord record;
  record.id = store_->allocate_evidence_id();
  record.key.kind = kind;
  record.key.subject = subject;
  record.key.source = source;
  // Controller-authored evidence always advances a single monotonic counter, so
  // two writes for the same key can never regress regardless of clock movement.
  (void)revision;
  record.revision = Revision::from_u64(++evidence_revision_);
  record.observed_at = now;
  record.ttl = ttl > 0 ? ttl : policy_.ttl_for(kind);
  if (record.ttl <= 0 || record.ttl > limits::kMaxEvidenceTtlNanos) {
    record.ttl = limits::kDefaultEvidenceTtlNanos;
  }
  record.klass = default_evidence_class(kind);
  record.payload = payload;
  record.observed_by = incarnation_;
  record.job = job;
  record.attempt = attempt;
  return record;
}

Status Controller::write_evidence_locked(EvidenceRecord record) {
  return store_->observe_evidence(std::move(record));
}

Status Controller::commit_job_locked(MaintenanceJob job, Nanos now) {
  const MaintenanceJob* current = store_->state().jobs.find(job.id);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job.id) + " does not exist");
  }
  job.revision = current->revision.next();
  job.updated_at = now;
  return store_->commit_job(std::move(job));
}

void Controller::new_attempt_locked(MaintenanceJob& job, Nanos now) const {
  AttemptId attempt;
  attempt.job = job.id;
  attempt.generation = job.generation;
  attempt.ordinal = job.next_attempt;
  job.next_attempt = job.next_attempt.next();
  job.active_attempt = attempt;
  job.owner = incarnation_;
  AttemptRecord record;
  record.id = attempt;
  record.started_at = now;
  record.outcome = AttemptOutcome::Running;
  record.detail = "attempt started by " + to_string(incarnation_);
  job.attempts.push_back(std::move(record));
  if (job.attempts.size() > limits::kMaxAttemptsPerJob) {
    const std::size_t excess = job.attempts.size() - limits::kMaxAttemptsPerJob;
    job.attempts.erase(job.attempts.begin(),
                       job.attempts.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

bool Controller::transition_locked(MaintenanceJob& job, JobState to, BlockReason reason,
                                   std::string detail, Nanos now, PassContext& context) {
  const TransitionCheck check = check_transition(job.state, to, job.service_removed);
  if (!check.allowed) {
    return false;
  }
  const JobState from = job.state;
  job.state = to;
  if (reason == BlockReason::None) {
    job.block = BlockReason::None;
    job.block_detail.clear();
  } else {
    job.block = reason;
    job.block_detail = detail;
  }
  job.last_detail = detail;
  HistoryEntry entry;
  entry.at = now;
  entry.from = from;
  entry.to = to;
  entry.reason = reason;
  entry.detail = std::move(detail);
  entry.by = incarnation_;
  job.push_history(std::move(entry));

  switch (to) {
    case JobState::Blocked: ++stats_.blocks; break;
    case JobState::InMaintenance:
      job.service_removed = true;
      job.started_at = now;
      ++stats_.starts;
      break;
    case JobState::Verifying: job.verification_started_at = now; break;
    case JobState::Restoring: job.restoration_started_at = now; break;
    case JobState::Complete:
      job.completed_at = now;
      ++stats_.completions;
      break;
    case JobState::Failed:
      job.completed_at = now;
      ++stats_.failures;
      break;
    case JobState::Cancelled:
      job.completed_at = now;
      ++stats_.cancellations;
      break;
    default: break;
  }

  if (is_terminal(to) || (to == JobState::Blocked && !job.service_removed)) {
    release_resources_locked(job, now, context);
  }
  if (is_terminal(to)) {
    for (AttemptRecord& attempt : job.attempts) {
      if (attempt.id == job.active_attempt && attempt.outcome == AttemptOutcome::Running) {
        attempt.outcome = to == JobState::Complete
                              ? AttemptOutcome::Succeeded
                              : (to == JobState::Failed ? AttemptOutcome::Failed
                                                        : AttemptOutcome::Cancelled);
        attempt.ended_at = now;
      }
    }
    job.active_attempt = AttemptId{};
  }
  return true;
}

void Controller::release_resources_locked(MaintenanceJob& job, Nanos now, PassContext& context) {
  (void)now;
  if (job.authority.valid()) {
    const Status released = store_->release_authority(job.authority, incarnation_);
    if (released.ok() || released.error().code == ErrorCode::NotFound ||
        released.error().code == ErrorCode::StaleIncarnation) {
      job.authority = AuthorityId{};
    }
  }
  if (job.lease.valid()) {
    if (context.actions != nullptr) {
      DrainAction action;
      action.kind = DrainAction::Kind::Release;
      action.job = job.id;
      action.generation = job.generation;
      action.attempt = job.active_attempt;
      action.lease = job.lease;
      action.reason = "job retired before service removal";
      context.actions->push_back(std::move(action));
    }
    job.lease = DrainLeaseId{};
  }
}

Controller::Evaluation Controller::evaluate_locked(const MaintenanceJob& job,
                                                   const ReadinessView& readiness,
                                                   bool check_authority, bool check_drain,
                                                   Nanos now) const {
  Evaluation evaluation;
  evaluation.readiness = readiness;
  ImpactInput input;
  input.topology = &topology_;
  input.policy = &policy_;
  input.readiness = &readiness;
  input.planned_removed = job.removal_set;
  input.baseline_removed = store_->state().ledger.reserved_targets(now);
  for (const auto& [id, other] : store_->state().jobs.jobs()) {
    // Only capacity that is genuinely out of service counts as a baseline.
    // A job that is still planning does not remove anything, its granted
    // authority is already represented in the ledger, and a job that has
    // completed has returned its scope to service.
    if (id == job.id || !stage_removes_service(other.state)) {
      continue;
    }
    for (const TargetId target : other.removal_set) {
      if (!contains(input.baseline_removed, target)) {
        input.baseline_removed.push_back(target);
      }
    }
  }
  evaluation.impact = compute_impact(input);

  PreconditionContext context;
  context.job = &job;
  context.topology = &topology_;
  context.policy = &policy_;
  context.evidence = &store_->state().evidence;
  context.ledger = &store_->state().ledger;
  context.jobs = &store_->state().jobs;
  context.readiness = &readiness;
  context.current = incarnation_;
  context.now = now;
  context.check_authority = check_authority;
  context.check_drain = check_drain;
  context.impact_in = &evaluation.impact;
  evaluation.preconditions = evaluate_preconditions(context);
  return evaluation;
}

bool Controller::drive_validated_locked(MaintenanceJob& job, PassContext& context) {
  for (const TargetId target : job.targets) {
    const TargetRecord* record = topology_.target(target);
    if (record == nullptr) {
      return transition_locked(job, JobState::Blocked, BlockReason::TargetUnknown,
                               "target " + to_string(target) + " is not declared in the topology",
                               context.now, context);
    }
    if (!record->maintainable) {
      return transition_locked(job, JobState::Blocked, BlockReason::TargetNotMaintainable,
                               "target " + to_string(target) + " is marked not maintainable",
                               context.now, context);
    }
  }
  if (!policy_.tiers.empty() && !policy_.known_tier(job.priority)) {
    return transition_locked(job, JobState::Blocked, BlockReason::PolicyDenied,
                             "priority tier " + std::to_string(job.priority) +
                                 " is not declared by policy",
                             context.now, context);
  }
  job.removal_set = topology_.removal_set(job.targets);
  job.policy_revision = policy_.revision;
  job.policy_digest = policy_.digest();
  job.topology_revision = topology_.revision();
  job.topology_digest = topology_.digest();
  job.owner = incarnation_;
  if (!job.active_attempt.valid()) {
    new_attempt_locked(job, context.now);
  }
  return transition_locked(job, JobState::Prerequisites, BlockReason::None,
                           "scope expanded to " + std::to_string(job.removal_set.size()) +
                               " target(s); prerequisites begin",
                           context.now, context);
}

void Controller::arbitrate_locked(PassContext& context, std::vector<DrainAction>& actions) {
  (void)actions;
  std::vector<JobId> candidates;
  for (const auto& [id, job] : store_->state().jobs.jobs()) {
    if (job.state != JobState::Prerequisites) {
      continue;
    }
    if (!job.active_attempt.valid()) {
      continue;
    }
    const AuthorityReservation* held =
        store_->state().ledger.find_for_job(job.id, job.generation);
    if (held != nullptr && held->live_at(context.now) && held->attempt == job.active_attempt) {
      continue;
    }
    candidates.push_back(id);
    if (candidates.size() >= limits::kMaxJobsPerArbitration) {
      break;
    }
  }
  if (candidates.empty()) {
    return;
  }

  ArbitrationContext arbitration;
  arbitration.topology = &topology_;
  arbitration.policy = &policy_;
  arbitration.readiness = context.readiness;
  arbitration.jobs = &store_->state().jobs;
  arbitration.candidates = candidates;
  arbitration.current = incarnation_;
  arbitration.now = context.now;

  AuthorityLedger staging = store_->state().ledger;
  arbitration.ledger = &staging;
  const ArbitrationReport report = arbitrate(arbitration);
  last_arbitration_ = report;

  for (const ArbitrationDecision& decision : report.decisions) {
    if (decision.outcome == ArbitrationOutcome::Denied) {
      const MaintenanceJob* denied = store_->state().jobs.find(decision.job);
      if (denied == nullptr || denied->state != JobState::Prerequisites ||
          denied->authority.valid()) {
        continue;
      }
      MaintenanceJob blocked = *denied;
      PassContext denial;
      denial.now = context.now;
      std::vector<DrainAction> unused;
      denial.actions = &unused;
      (void)transition_locked(blocked, JobState::Blocked, decision.reason, decision.detail,
                              context.now, denial);
      (void)commit_job_locked(std::move(blocked), context.now);
      continue;
    }
    if (decision.outcome != ArbitrationOutcome::Granted) {
      continue;
    }
    const MaintenanceJob* current = store_->state().jobs.find(decision.job);
    if (current == nullptr) {
      continue;
    }
    const AuthorityReservation* granted = staging.find(decision.authority);
    if (granted == nullptr) {
      continue;
    }
    const Status reserved = store_->insert_authority(*granted);
    if (!reserved.ok()) {
      continue;
    }
    MaintenanceJob job = *current;
    job.authority = decision.authority;
    job.preconditions_at = context.now;
    job.preconditions_digest = decision.impact_digest;
    job.last_detail = "maintenance authority " + to_string(decision.authority) +
                      " granted by arbitration at position " +
                      std::to_string(decision.position) + " [" + decision.ordering_key + "]";
    (void)commit_job_locked(std::move(job), context.now);
  }
}

bool Controller::drive_prerequisites_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  if (!job.active_attempt.valid()) {
    new_attempt_locked(job, now);
    return true;
  }
  if (!job.authority.valid()) {
    // The arbitrator grants authority; until then the job simply waits.
    return false;
  }

  const AuthorityReservation* held = store_->state().ledger.find(job.authority);
  if (held == nullptr) {
    job.authority = AuthorityId{};
    return true;
  }
  const Nanos margin = renewal_margin(policy_);
  if (held->expires_at - now <= margin) {
    const Status renewed =
        store_->renew_authority(job.authority, job.active_attempt, incarnation_, margin, now);
    if (!renewed.ok()) {
      if (renewed.error().code == ErrorCode::NotFound ||
          renewed.error().code == ErrorCode::StaleIncarnation ||
          renewed.error().code == ErrorCode::StaleAttempt) {
        job.authority = AuthorityId{};
        ++stats_.authority_revocations;
        return true;
      }
      return transition_locked(job, JobState::Blocked, BlockReason::AuthorityDenied,
                               renewed.error().detail, now, context);
    }
  }

  if (job.requires_drain && !job.lease.valid()) {
    if (job.drain_failures >= policy_.max_drain_failures_before_block) {
      return transition_locked(job, JobState::Blocked, BlockReason::DrainFailed,
                               "drain failed " + std::to_string(job.drain_failures) +
                                   " time(s); policy bound is " +
                                   std::to_string(policy_.max_drain_failures_before_block),
                               now, context);
    }
    if (drain_inflight_.find(job.id) == drain_inflight_.end()) {
      DrainAction action;
      action.kind = DrainAction::Kind::Request;
      action.job = job.id;
      action.generation = job.generation;
      action.attempt = job.active_attempt;
      action.targets = job.removal_set;
      action.lease_duration = policy_.drain_lease;
      action.reason = job.title;
      context.actions->push_back(std::move(action));
      drain_inflight_.insert(job.id);
      ++stats_.drain_requests;
    }
    return false;
  }

  const Evaluation evaluation = evaluate_locked(job, *context.readiness, true, true, now);
  if (evaluation.preconditions.violated > 0) {
    const BlockReason reason =
        evaluation.preconditions.first_violation().value_or(BlockReason::PolicyDenied);
    const PreconditionResult* first = nullptr;
    for (const PreconditionResult& result : evaluation.preconditions.results) {
      if (result.status == PreconditionStatus::Violated) {
        first = &result;
        break;
      }
    }
    return transition_locked(
        job, JobState::Blocked, reason,
        first != nullptr ? first->detail : std::string("precondition violated"), now, context);
  }
  if (evaluation.preconditions.pending > 0) {
    job.preconditions_at = now;
    job.preconditions_digest = evaluation.preconditions.digest;
    job.preconditions_satisfied = false;
    job.last_detail = "waiting for prerequisites: " +
                      std::to_string(evaluation.preconditions.pending) + " pending";
    return true;
  }
  job.preconditions_at = now;
  job.preconditions_digest = evaluation.preconditions.digest;
  job.preconditions_policy_revision = evaluation.preconditions.policy_revision;
  job.preconditions_topology_revision = evaluation.preconditions.topology_revision;
  job.preconditions_satisfied = true;
  return transition_locked(job, JobState::Ready, BlockReason::None,
                           "every precondition authoritatively satisfied (digest " +
                               hex_u64(evaluation.preconditions.digest) + ")",
                           now, context);
}

bool Controller::drive_ready_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  const Evaluation evaluation = evaluate_locked(job, *context.readiness, true, true, now);
  if (evaluation.preconditions.violated > 0) {
    const BlockReason reason =
        evaluation.preconditions.first_violation().value_or(BlockReason::PolicyDenied);
    const PreconditionResult* first = nullptr;
    for (const PreconditionResult& result : evaluation.preconditions.results) {
      if (result.status == PreconditionStatus::Violated) {
        first = &result;
        break;
      }
    }
    return transition_locked(
        job, JobState::Blocked, reason,
        first != nullptr ? first->detail : std::string("precondition violated"), now, context);
  }
  if (evaluation.preconditions.pending > 0) {
    return transition_locked(job, JobState::Prerequisites, BlockReason::None,
                             "prerequisites are no longer fully satisfied", now, context);
  }
  if (now - job.preconditions_at > policy_.max_precondition_age) {
    return transition_locked(job, JobState::Prerequisites, BlockReason::EvidenceMissingOrStale,
                             "the recorded precondition evaluation is " +
                                 format_duration(now - job.preconditions_at) +
                                 " old and exceeds the policy bound of " +
                                 format_duration(policy_.max_precondition_age),
                             now, context);
  }

  const WindowEvaluation window =
      evaluate_window(policy_.windows, job.targets, now, policy_.require_window);
  job.window = window.window;
  job.window_closes_at = window.verdict == WindowVerdict::Open ? window.closes_at : 0;
  job.on_close = OnWindowClose::FinishActiveStep;
  if (window.verdict == WindowVerdict::Open) {
    if (const Window* entry = policy_.window(window.window); entry != nullptr) {
      job.on_close = entry->on_close;
    }
  }
  job.preconditions_digest = evaluation.preconditions.digest;
  job.preconditions_at = now;
  return transition_locked(job, JobState::InMaintenance, BlockReason::None,
                           "service removal begins under window " + to_string(window.window) +
                               " (verdict " + to_string(window.verdict) + ")",
                           now, context);
}

bool Controller::drive_blocked_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  const bool policy_changed = job.policy_revision.valid() && job.policy_revision != policy_.revision;
  const bool topology_changed =
      job.topology_revision.valid() && job.topology_revision != topology_.revision();
  if (is_hard_block(job.block) && !policy_changed && !topology_changed) {
    return false;
  }
  if (now - job.updated_at < kSoftBlockDwellNanos && !policy_changed && !topology_changed) {
    return false;
  }
  ++stats_.unblocks;
  const std::string previous = to_string(job.block);
  if (job.service_removed) {
    if (!job.active_attempt.valid()) {
      new_attempt_locked(job, now);
    }
    return transition_locked(job, JobState::Verifying, job.block,
                             "resuming after block '" + previous +
                                 "' with service already removed; verification is mandatory",
                             now, context);
  }
  return transition_locked(job, JobState::Prerequisites, BlockReason::None,
                           "resuming after block '" + previous + "'", now, context);
}

bool Controller::completion_confirmed_locked(const MaintenanceJob& job, Nanos now,
                                             bool& any_failed) const {
  any_failed = false;
  if (job.removal_set.empty()) {
    return true;
  }
  bool all = true;
  for (const TargetId target : job.removal_set) {
    bool found = false;
    for (std::uint64_t source = 1; source <= limits::kMaxEvidenceSourcesPerKey; ++source) {
      EvidenceQuery query;
      query.key.kind = EvidenceKind::MaintenanceCompletion;
      query.key.subject = EvidenceSubject::of(target);
      query.key.source = SourceId::from_u64(source);
      query.current = incarnation_;
      query.now = now;
      query.job = job.id;
      query.attempt = job.active_attempt;
      const EvidenceRecord* record = store_->state().evidence.find(query.key);
      if (record == nullptr) {
        continue;
      }
      if (!store_->state().evidence.evaluate(query).fresh()) {
        continue;
      }
      found = true;
      if (!record->payload.result) {
        any_failed = true;
      }
      break;
    }
    if (!found) {
      all = false;
    }
  }
  return all;
}

bool Controller::drive_in_maintenance_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  const bool window_closed = job.window_closes_at > 0 && now >= job.window_closes_at;

  bool any_failed = false;
  const bool complete = completion_confirmed_locked(job, now, any_failed);

  if (window_closed) {
    if (job.on_close == OnWindowClose::AbortAndRestore) {
      const Window* window = policy_.window(job.window);
      const Nanos grace =
          window != nullptr && window->abort_grace > 0 ? window->abort_grace : kAbortGraceFallbackNanos;
      if (!complete && now > job.window_closes_at + grace) {
        quarantine_targets_locked(job, BlockReason::WindowUnavailable,
                                  "window closed while the maintenance action was running and the "
                                  "attempt did not report completion within the abort grace",
                                  now);
        return transition_locked(job, JobState::Failed, BlockReason::WindowUnavailable,
                                 "abort-and-restore deadline expired without a completion report",
                                 now, context);
      }
    } else if (job.on_close == OnWindowClose::ExtendToComplete) {
      if (!job.window_extended) {
        job.window_extended = true;
        job.last_detail = "policy extended the window so the active step can complete";
        return true;
      }
    } else if (!job.overrun) {
      job.overrun = true;
      job.last_detail =
          "window closed while the active step was running; the step is allowed to finish and the "
          "overrun is recorded";
      return true;
    }
  }

  if (!complete) {
    return false;
  }
  if (any_failed) {
    job.maintenance_failed = true;
  }
  return transition_locked(job, JobState::Verifying, BlockReason::None,
                           any_failed
                               ? "the maintenance action reported failure; the scope is verified "
                                 "and restored before the job closes as failed"
                               : "maintenance action complete; post-maintenance verification begins",
                           now, context);
}

bool Controller::drive_verifying_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  std::vector<TargetId> gaps;
  for (const TargetId target : job.removal_set) {
    if (!context.readiness->is_fresh(target) ||
        !is_serving(context.readiness->health_of(target))) {
      gaps.push_back(target);
    }
  }

  bool negative_report = false;
  for (const TargetId target : job.removal_set) {
    for (std::uint64_t source = 1; source <= limits::kMaxEvidenceSourcesPerKey; ++source) {
      EvidenceQuery query;
      query.key.kind = EvidenceKind::RestorationCheck;
      query.key.subject = EvidenceSubject::of(target);
      query.key.source = SourceId::from_u64(source);
      query.current = incarnation_;
      query.now = now;
      query.job = job.id;
      query.attempt = job.active_attempt;
      const EvidenceRecord* record = store_->state().evidence.find(query.key);
      if (record == nullptr) {
        continue;
      }
      if (store_->state().evidence.evaluate(query).fresh() && !record->payload.result) {
        negative_report = true;
        break;
      }
    }
    if (negative_report) {
      break;
    }
  }

  if (!gaps.empty() || negative_report) {
    const Nanos round = now - job.verification_started_at;
    if (round > job.estimated_duration) {
      ++job.verification_failures;
      ++stats_.verifications_failed;
      if (job.verification_failures >= policy_.max_verification_attempts) {
        quarantine_targets_locked(job, BlockReason::VerificationFailed,
                                  "post-maintenance verification never proved the scope healthy",
                                  now);
        return transition_locked(job, JobState::Failed, BlockReason::VerificationFailed,
                                 "verification failed after " +
                                     std::to_string(job.verification_failures) + " round(s)",
                                 now, context);
      }
      job.verification_started_at = now;
      job.last_detail = "verification round " + std::to_string(job.verification_failures) +
                        " exhausted; retrying with fresh evidence";
      return true;
    }
    job.last_detail = negative_report
                          ? "a restoration check reported failure for the scope"
                          : "waiting for fresh healthy evidence for " +
                                std::to_string(gaps.size()) + " target(s)";
    return true;
  }

  ++job.verification_passes;
  ++stats_.verifications_passed;
  return transition_locked(job, JobState::Restoring, BlockReason::None,
                           "post-maintenance verification passed with fresh evidence for " +
                               std::to_string(job.removal_set.size()) + " target(s)",
                           now, context);
}

bool Controller::drive_restoring_locked(MaintenanceJob& job, PassContext& context) {
  const Nanos now = context.now;
  if (job.lease.valid()) {
    if (drain_inflight_.find(job.id) == drain_inflight_.end() &&
        queued_releases_.find(job.lease) == queued_releases_.end()) {
      DrainAction action;
      action.kind = DrainAction::Kind::Release;
      action.job = job.id;
      action.generation = job.generation;
      action.attempt = job.active_attempt;
      action.lease = job.lease;
      action.reason = "restoration complete";
      context.actions->push_back(std::move(action));
      queued_releases_.insert(job.lease);
    }
    return false;
  }

  std::vector<TargetId> gaps;
  for (const TargetId target : job.removal_set) {
    if (store_->state().quarantines.find_open_for_target(target) != nullptr) {
      gaps.push_back(target);
      continue;
    }
    if (!context.readiness->is_fresh(target) ||
        !is_serving(context.readiness->health_of(target))) {
      gaps.push_back(target);
    }
  }
  if (!gaps.empty()) {
    const Nanos round = now - job.restoration_started_at;
    if (round > job.estimated_duration) {
      ++job.restoration_failures;
      ++stats_.restoration_failures;
      if (job.restoration_failures >= policy_.max_restoration_attempts) {
        quarantine_targets_locked(job, BlockReason::RestorationFailed,
                                  "restoration never proved the scope healthy", now);
        return transition_locked(job, JobState::Failed, BlockReason::RestorationFailed,
                                 "restoration failed for " + to_string(gaps.front()) + " (" +
                                     std::to_string(gaps.size()) + " target(s))",
                                 now, context);
      }
      job.restoration_started_at = now;
      job.last_detail = "restoration round " + std::to_string(job.restoration_failures) +
                        " exhausted; retrying with fresh evidence";
      return true;
    }
    job.last_detail =
        "waiting for restoration evidence for " + std::to_string(gaps.size()) + " target(s)";
    return true;
  }

  // The explicit restoration verdict is recorded before the job may complete.
  for (const TargetId target : job.removal_set) {
    EvidencePayload payload;
    payload.result = true;
    payload.health = context.readiness->health_of(target);
    EvidenceRecord record = make_evidence_locked(
        EvidenceKind::RestorationCheck, EvidenceSubject::of(target), SourceId::from_u64(1),
        Revision{}, now, policy_.ttl_for(EvidenceKind::RestorationCheck), payload, job.id,
        job.active_attempt);
    (void)write_evidence_locked(std::move(record));
  }

  ++stats_.restorations;
  if (job.maintenance_failed) {
    return transition_locked(job, JobState::Failed, BlockReason::VerificationFailed,
                             "service restored, but the maintenance action itself failed", now,
                             context);
  }
  return transition_locked(job, JobState::Complete, BlockReason::None,
                           "scope returned to service and verified", now, context);
}

void Controller::quarantine_targets_locked(const MaintenanceJob& job, BlockReason reason,
                                           std::string detail, Nanos now) {
  for (const TargetId target : job.removal_set) {
    Result<QuarantineId> created = store_->quarantine_target(
        target, job.id, job.generation, reason, detail, now, incarnation_);
    if (created.ok()) {
      ++stats_.quarantines;
    }
  }
}

bool Controller::drive_locked(JobId id, PassContext& context) {
  const MaintenanceJob* current = store_->state().jobs.find(id);
  if (current == nullptr || current->is_terminal()) {
    return false;
  }
  MaintenanceJob job = *current;
  bool changed = false;
  switch (job.state) {
    case JobState::Validated: changed = drive_validated_locked(job, context); break;
    case JobState::Prerequisites: changed = drive_prerequisites_locked(job, context); break;
    case JobState::Ready: changed = drive_ready_locked(job, context); break;
    case JobState::Blocked: changed = drive_blocked_locked(job, context); break;
    case JobState::InMaintenance: changed = drive_in_maintenance_locked(job, context); break;
    case JobState::Verifying: changed = drive_verifying_locked(job, context); break;
    case JobState::Restoring: changed = drive_restoring_locked(job, context); break;
    default: changed = false; break;
  }
  if (!changed) {
    return false;
  }
  return commit_job_locked(std::move(job), context.now).ok();
}

void Controller::drain_all_locked(PassContext& context, std::vector<DrainAction>& actions) {
  context.actions = &actions;
  ReadinessView readiness = ReadinessView::build(topology_, store_->state().evidence, policy_,
                                                 incarnation_, context.now);
  context.readiness = &readiness;

  const SchedulerSelection selection =
      plan_tick(SchedulerInputs{&store_->state().jobs, &policy_, context.now});

  std::vector<JobId> order;
  const auto append = [&order](const std::vector<JobId>& list) {
    for (const JobId id : list) {
      if (std::find(order.begin(), order.end(), id) == order.end()) {
        order.push_back(id);
      }
    }
  };
  append(selection.validated);
  append(selection.restoring);
  append(selection.verifying);
  append(selection.in_maintenance);
  append(selection.runnable);
  std::sort(order.begin(), order.end());

  for (const JobId id : order) {
    (void)drive_locked(id, context);
  }

  arbitrate_locked(context, actions);

  // Release drain leases no live job owns, so the external service is never
  // left draining a resource the runtime no longer tracks.
  for (const auto& [id, lease] : store_->state().leases) {
    bool owned = false;
    for (const auto& [job_id, job] : store_->state().jobs.jobs()) {
      (void)job_id;
      if (job.lease == id && !job.is_terminal()) {
        owned = true;
        break;
      }
    }
    if (owned || lease.state == DrainState::Released || queued_releases_.find(id) != queued_releases_.end()) {
      continue;
    }
    DrainAction action;
    action.kind = DrainAction::Kind::Release;
    action.lease = id;
    action.reason = "orphaned drain lease";
    actions.push_back(std::move(action));
    queued_releases_.insert(id);
  }
}

void Controller::perform_action(const DrainAction& action, std::vector<DrainOutcome>& outcomes) {
  DrainOutcome outcome;
  outcome.action = action;
  switch (action.kind) {
    case DrainAction::Kind::Request: {
      DrainRequest request;
      request.job = action.job;
      request.generation = action.generation;
      request.attempt = action.attempt;
      request.targets = action.targets;
      request.lease_duration = action.lease_duration;
      request.reason = action.reason;
      request.requester = incarnation_;
      outcome.response = drain_->request_drain(request);
      break;
    }
    case DrainAction::Kind::Release:
      outcome.response.status = drain_->release_drain(action.lease, incarnation_);
      outcome.response.lease.id = action.lease;
      outcome.response.lease.state = DrainState::Released;
      break;
    case DrainAction::Kind::Query:
      outcome.response = drain_->query_drain(action.lease, incarnation_);
      break;
  }
  outcomes.push_back(std::move(outcome));
}

void Controller::apply_outcomes_locked(const std::vector<DrainOutcome>& outcomes, Nanos now) {
  for (const DrainOutcome& outcome : outcomes) {
    const DrainAction& action = outcome.action;
    const DrainLeaseId lease_id =
        outcome.response.lease.id.valid() ? outcome.response.lease.id : action.lease;
    drain_inflight_.erase(action.job);

    if (action.kind == DrainAction::Kind::Release) {
      queued_releases_.erase(action.lease);
      if (outcome.response.status.ok() ||
          outcome.response.status.error().code == ErrorCode::NotFound) {
        (void)store_->commit_lease_erased(lease_id);
        ++stats_.drain_releases;
        // Whichever job referenced this lease must stop believing it holds one,
        // otherwise it would try to release it again on every pass.
        for (const auto& [id, candidate] : store_->state().jobs.jobs()) {
          if (candidate.lease != lease_id || candidate.is_terminal()) {
            continue;
          }
          MaintenanceJob updated = candidate;
          updated.lease = DrainLeaseId{};
          if (updated.last_detail.empty()) {
            updated.last_detail = "drain lease released";
          }
          (void)commit_job_locked(std::move(updated), now);
          (void)id;
          break;
        }
      }
      continue;
    }
    if (action.kind == DrainAction::Kind::Query) {
      if (!outcome.response.status.ok()) {
        (void)store_->commit_lease_erased(action.lease);
      }
      continue;
    }

    const MaintenanceJob* current = store_->state().jobs.find(action.job);
    const bool stale = current == nullptr || current->generation != action.generation ||
                       current->active_attempt != action.attempt;
    if (stale) {
      if (current != nullptr) {
        ++stats_.stale_generation_rejections;
      }
      // The actor that asked for the drain no longer exists: release whatever
      // the drain service granted instead of leaving capacity reserved.
      if (outcome.response.status.ok() && lease_id.valid()) {
        DrainAction release;
        release.kind = DrainAction::Kind::Release;
        release.lease = lease_id;
        release.reason = "drain granted to a fenced attempt";
        deferred_releases_.push_back(std::move(release));
        queued_releases_.insert(lease_id);
      }
      continue;
    }

    if (!outcome.response.status.ok()) {
      MaintenanceJob job = *current;
      ++job.drain_failures;
      ++stats_.drain_failures;
      job.last_detail = "drain request failed: " + format_error(outcome.response.status.error());
      if (job.drain_failures >= policy_.max_drain_failures_before_block) {
        PassContext context;
        context.now = now;
        (void)transition_locked(job, JobState::Blocked, BlockReason::DrainFailed, job.last_detail,
                                now, context);
      }
      (void)commit_job_locked(std::move(job), now);
      continue;
    }

    const DrainLease lease = outcome.response.lease;
    if (!lease.id.valid()) {
      continue;
    }
    const Status committed = store_->commit_lease(lease);
    if (!committed.ok()) {
      continue;
    }
    MaintenanceJob job = *current;
    job.lease = lease.id;
    job.last_detail = "drain lease " + to_string(lease.id) + " granted in state " +
                      to_string(lease.state);
    if (lease.state == DrainState::Drained || lease.state == DrainState::Draining) {
      for (const TargetId target : job.removal_set) {
        EvidencePayload payload;
        payload.result = lease.state == DrainState::Drained;
        payload.health = Health::Down;
        EvidenceRecord record = make_evidence_locked(
            EvidenceKind::DrainLeaseState, EvidenceSubject::of(target), SourceId::from_u64(1),
            Revision{}, now, policy_.ttl_for(EvidenceKind::DrainLeaseState), payload, job.id,
            job.active_attempt);
        (void)write_evidence_locked(std::move(record));
      }
    }
    (void)commit_job_locked(std::move(job), now);
  }
}

Status Controller::tick(Nanos now) {
  for (int pass = 0; pass < kMaxPassesPerTick; ++pass) {
    std::vector<DrainAction> actions;
    {
      std::lock_guard<std::mutex> guard(mu_);
      if (stopping_) {
        return make_error(ErrorCode::ShuttingDown, "controller is shutting down");
      }
      ++stats_.ticks;
      (void)store_->expire_authority(now);
      actions.insert(actions.end(), deferred_releases_.begin(), deferred_releases_.end());
      deferred_releases_.clear();
      PassContext context;
      context.now = now;
      drain_all_locked(context, actions);
    }
    if (actions.empty()) {
      break;
    }
    for (const DrainAction& action : actions) {
      {
        std::lock_guard<std::mutex> guard(mu_);
        if (action.job.valid()) {
          drain_inflight_.insert(action.job);
        }
      }
      std::vector<DrainOutcome> outcomes;
      perform_action(action, outcomes);
      std::lock_guard<std::mutex> guard(mu_);
      apply_outcomes_locked(outcomes, now);
    }
  }
  std::lock_guard<std::mutex> guard(mu_);
  if (store_->should_compact()) {
    const Status compacted = store_->compact(now);
    if (compacted.ok()) {
      ++stats_.compactions;
    }
  }
  return ok_status();
}

Status Controller::tick() { return tick(clock_->now()); }

Result<JobId> Controller::propose(MaintenanceRequest request, std::string actor) {
  const Status shape = validate_request_shape(request);
  if (!shape.ok()) {
    return shape.error();
  }
  std::lock_guard<std::mutex> guard(mu_);
  if (stopping_) {
    return make_error(ErrorCode::ShuttingDown, "controller is shutting down");
  }
  if (!policy_.tiers.empty() && !policy_.known_tier(request.priority)) {
    return make_error(ErrorCode::PolicyDenied, "priority tier " +
                                                   std::to_string(request.priority) +
                                                   " is not declared by policy");
  }
  for (const JobId dependency : request.depends_on) {
    const MaintenanceJob* other = store_->state().jobs.find(dependency);
    if (other == nullptr) {
      return make_error(ErrorCode::NotFound,
                        "dependency " + to_string(dependency) + " does not exist");
    }
    // Bounded transitive walk: a pre-existing cycle in the dependency graph is
    // rejected rather than inherited.
    std::vector<JobId> frontier{dependency};
    std::vector<JobId> seen{dependency};
    std::size_t budget = limits::kMaxDependenciesPerJob * limits::kMaxDependenciesPerJob;
    while (!frontier.empty()) {
      if (budget-- == 0) {
        return make_error(ErrorCode::StateConflict, "dependency graph exceeds the search budget");
      }
      const JobId current_id = frontier.back();
      frontier.pop_back();
      const MaintenanceJob* node = store_->state().jobs.find(current_id);
      if (node == nullptr) {
        continue;
      }
      for (const JobId next : node->depends_on) {
        if (next == dependency) {
          return make_error(ErrorCode::StateConflict,
                            "dependency graph containing " + to_string(dependency) +
                                " is cyclic");
        }
        if (std::find(seen.begin(), seen.end(), next) == seen.end()) {
          seen.push_back(next);
          frontier.push_back(next);
        }
      }
    }
  }

  MaintenanceJob job;
  job.generation = GenerationId::from_u64(1);
  job.title = request.title;
  job.reason = request.reason;
  job.targets = request.targets;
  job.depends_on = request.depends_on;
  job.window_filter = request.window_filter;
  job.priority = request.priority;
  job.estimated_duration =
      request.estimated_duration > 0 ? request.estimated_duration : policy_.default_duration;
  job.requires_drain = request.requires_drain;
  job.requestor = request.requestor;
  job.state = JobState::Proposed;
  job.created_at = clock_->now();
  job.updated_at = job.created_at;
  job.owner = incarnation_;
  job.approved_by = std::move(actor);

  Result<JobId> created = store_->create_job(std::move(job));
  if (!created.ok()) {
    return created.error();
  }
  ++stats_.proposals;
  return created.value();
}

Status Controller::approve(JobId job, std::string actor) {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  if (current->state != JobState::Proposed) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) + " is " + to_string(current->state) +
                          ", only a proposed job can be approved");
  }
  MaintenanceJob updated = *current;
  updated.state = JobState::Validated;
  updated.approved = true;
  updated.approved_by = std::move(actor);
  HistoryEntry entry;
  entry.at = clock_->now();
  entry.from = JobState::Proposed;
  entry.to = JobState::Validated;
  entry.detail = "approved by " + updated.approved_by;
  entry.by = incarnation_;
  updated.push_history(std::move(entry));
  const Status committed = commit_job_locked(std::move(updated), clock_->now());
  if (!committed.ok()) {
    return committed;
  }
  ++stats_.approvals;
  return ok_status();
}

Status Controller::rearm(JobId job, std::string actor) {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  if (current->service_removed) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) +
                          " has already removed service; it must be restored, not re-armed");
  }
  if (current->is_terminal()) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) + " is terminal and cannot be re-armed");
  }
  const Nanos now = clock_->now();
  MaintenanceJob updated = *current;
  std::vector<DrainAction> actions;
  PassContext context;
  context.now = now;
  context.actions = &actions;
  release_resources_locked(updated, now, context);
  for (AttemptRecord& attempt : updated.attempts) {
    if (attempt.outcome == AttemptOutcome::Running) {
      attempt.outcome = AttemptOutcome::Fenced;
      attempt.ended_at = now;
      attempt.detail =
          "fenced by re-arm under generation " + to_string(updated.generation.next());
    }
  }
  updated.generation = updated.generation.next();
  updated.active_attempt = AttemptId{};
  updated.preconditions_satisfied = false;
  updated.preconditions_at = 0;
  updated.preconditions_digest = 0;
  updated.overrun = false;
  updated.window_extended = false;
  updated.window_closes_at = 0;
  updated.verification_failures = 0;
  updated.restoration_failures = 0;
  updated.drain_failures = 0;
  updated.maintenance_failed = false;
  updated.block = BlockReason::None;
  updated.block_detail.clear();
  updated.approved_by = std::move(actor);

  const JobState target_state =
      updated.state == JobState::Proposed ? JobState::Validated : JobState::Prerequisites;
  const TransitionCheck check = check_transition(updated.state, target_state, false);
  if (!check.allowed) {
    return make_error(ErrorCode::StateConflict, check.reason);
  }
  const JobState from = updated.state;
  updated.state = target_state;
  updated.last_detail = "re-armed as generation " + to_string(updated.generation);
  HistoryEntry entry;
  entry.at = now;
  entry.from = from;
  entry.to = target_state;
  entry.detail = updated.last_detail;
  entry.by = incarnation_;
  updated.push_history(std::move(entry));

  const Status committed = commit_job_locked(std::move(updated), now);
  if (!committed.ok()) {
    return committed;
  }
  for (DrainAction& action : actions) {
    queued_releases_.insert(action.lease);
    deferred_releases_.push_back(std::move(action));
  }
  ++stats_.rearmings;
  return ok_status();
}

Status Controller::cancel(JobId job, std::string actor, std::string reason) {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  if (current->service_removed) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) +
                          " has removed service; cancellation is refused so the scope must be "
                          "restored instead");
  }
  if (current->is_terminal()) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) + " is already " + to_string(current->state));
  }
  MaintenanceJob updated = *current;
  std::vector<DrainAction> actions;
  PassContext context;
  context.now = clock_->now();
  context.actions = &actions;
  if (!transition_locked(updated, JobState::Cancelled, BlockReason::None,
                         "cancelled by " + actor + ": " + reason, context.now, context)) {
    return make_error(ErrorCode::StateConflict, "cancellation is not a legal transition");
  }
  const Status committed = commit_job_locked(std::move(updated), context.now);
  if (!committed.ok()) {
    return committed;
  }
  for (DrainAction& action : actions) {
    queued_releases_.insert(action.lease);
    deferred_releases_.push_back(std::move(action));
  }
  return ok_status();
}

Status Controller::observe(EvidenceRecord evidence) {
  std::lock_guard<std::mutex> guard(mu_);
  if (stopping_) {
    return make_error(ErrorCode::ShuttingDown, "controller is shutting down");
  }
  if (!evidence.observed_by.valid()) {
    evidence.observed_by = incarnation_;
  }
  if (!evidence.id.valid()) {
    evidence.id = store_->allocate_evidence_id();
  }
  if (evidence.ttl <= 0 || evidence.ttl > limits::kMaxEvidenceTtlNanos) {
    evidence.ttl = policy_.ttl_for(evidence.key.kind);
  }
  const Status observed = store_->observe_evidence(std::move(evidence));
  if (!observed.ok() && observed.error().code == ErrorCode::StaleRevision) {
    ++stats_.stale_evidence_rejections;
  }
  return observed;
}

Status Controller::report_completion(JobId job, const AttemptId& attempt, bool succeeded,
                                     std::string detail) {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  if (attempt.generation != current->generation) {
    ++stats_.stale_generation_rejections;
    return make_error(ErrorCode::StaleGeneration,
                      "completion report carries generation " + to_string(attempt.generation) +
                          " but the job is at generation " + to_string(current->generation));
  }
  if (!current->active_attempt.valid() || attempt != current->active_attempt) {
    ++stats_.stale_attempt_rejections;
    return make_error(ErrorCode::StaleAttempt,
                      "completion report carries attempt " + to_string(attempt) +
                          " but the current attempt is " + to_string(current->active_attempt));
  }
  if (current->state != JobState::InMaintenance) {
    return make_error(ErrorCode::StateConflict,
                      "job " + to_string(job) + " is " + to_string(current->state) +
                          " and cannot accept a completion report");
  }
  const Nanos now = clock_->now();
  for (const TargetId target : current->removal_set) {
    EvidencePayload payload;
    payload.result = succeeded;
    payload.health = succeeded ? Health::Degraded : Health::Down;
    EvidenceRecord record = make_evidence_locked(
        EvidenceKind::MaintenanceCompletion, EvidenceSubject::of(target), SourceId::from_u64(1),
        Revision{}, now, policy_.ttl_for(EvidenceKind::MaintenanceCompletion), payload, job,
        attempt);
    const Status written = write_evidence_locked(std::move(record));
    if (!written.ok()) {
      return written;
    }
  }
  MaintenanceJob updated = *current;
  updated.last_detail = "completion reported: " + std::move(detail);
  return commit_job_locked(std::move(updated), now);
}

Status Controller::report_restoration(JobId job, const AttemptId& attempt, TargetId target, bool ok,
                                      std::string detail) {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  if (attempt.generation != current->generation) {
    ++stats_.stale_generation_rejections;
    return make_error(ErrorCode::StaleGeneration,
                      "restoration report carries generation " + to_string(attempt.generation) +
                          " but the job is at generation " + to_string(current->generation));
  }
  if (!current->active_attempt.valid() || attempt != current->active_attempt) {
    ++stats_.stale_attempt_rejections;
    return make_error(ErrorCode::StaleAttempt,
                      "restoration report carries attempt " + to_string(attempt) +
                          " but the current attempt is " + to_string(current->active_attempt));
  }
  if (!contains(current->removal_set, target)) {
    return invalid_argument("target " + to_string(target) + " is not in the removal set of job " +
                            to_string(job));
  }
  const Nanos now = clock_->now();
  EvidencePayload payload;
  payload.result = ok;
  payload.health = ok ? Health::Healthy : Health::Down;
  EvidenceRecord record = make_evidence_locked(
      EvidenceKind::RestorationCheck, EvidenceSubject::of(target), SourceId::from_u64(2),
      Revision{}, now, policy_.ttl_for(EvidenceKind::RestorationCheck), payload, job, attempt);
  const Status written = write_evidence_locked(std::move(record));
  if (!written.ok()) {
    return written;
  }
  MaintenanceJob updated = *current;
  updated.last_detail = "restoration reported: " + std::move(detail);
  return commit_job_locked(std::move(updated), now);
}

Status Controller::clear_quarantine(QuarantineId id, std::string actor, std::string justification) {
  std::lock_guard<std::mutex> guard(mu_);
  const QuarantineRecord* record = store_->state().quarantines.find(id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "quarantine " + to_string(id) + " does not exist");
  }
  QuarantineRecord updated = *record;
  updated.cleared = true;
  updated.cleared_at = clock_->now();
  updated.cleared_by = std::move(actor) + ": " + std::move(justification);
  return store_->commit_quarantine(std::move(updated));
}

Status Controller::reconcile_locked(Nanos now) {
  std::vector<AuthorityReservation> revoked = store_->fence_authority(incarnation_);
  stats_.authority_revocations += revoked.size();

  std::vector<MaintenanceJob> updates;
  std::vector<DrainAction> releases;
  for (const auto& [id, job] : store_->state().jobs.jobs()) {
    (void)id;
    if (job.is_terminal() || job.owner == incarnation_) {
      continue;
    }
    if (job.state == JobState::Proposed || job.state == JobState::Validated) {
      continue;
    }
    MaintenanceJob updated = job;
    const bool removed = updated.service_removed;
    for (AttemptRecord& attempt : updated.attempts) {
      if (attempt.outcome == AttemptOutcome::Running) {
        attempt.outcome = AttemptOutcome::Fenced;
        attempt.ended_at = now;
        attempt.detail = "fenced by controller restart; incarnation " + to_string(incarnation_) +
                         " replaced " + to_string(updated.owner);
      }
    }
    updated.active_attempt = AttemptId{};
    updated.authority = AuthorityId{};
    updated.preconditions_satisfied = false;
    updated.preconditions_at = 0;
    updated.preconditions_digest = 0;
    updated.owner = incarnation_;
    updated.overrun = false;
    updated.verification_started_at = now;
    updated.restoration_started_at = now;

    if (removed) {
      // A new incarnation drives the scope with a new attempt, so every token
      // issued by the previous process is fenced.
      new_attempt_locked(updated, now);
      AuthorityReservation reservation;
      reservation.job = updated.id;
      reservation.generation = updated.generation;
      reservation.attempt = updated.active_attempt;
      reservation.owner = incarnation_;
      reservation.targets = updated.removal_set;
      std::sort(reservation.targets.begin(), reservation.targets.end());
      reservation.domains = topology_.domains_of(reservation.targets);
      reservation.pools = topology_.pools_of(reservation.targets);
      reservation.granted_at = now;
      reservation.expires_at =
          now + std::clamp(policy_.authority_lease, limits::kMinDrainLeaseNanos,
                           limits::kMaxAuthorityLeaseNanos);
      Result<AuthorityId> reissued = store_->reserve_authority(std::move(reservation));
      if (reissued.ok()) {
        updated.authority = reissued.value();
        ++stats_.authority_reissues;
      }
    } else if (updated.lease.valid()) {
      DrainAction action;
      action.kind = DrainAction::Kind::Release;
      action.job = updated.id;
      action.lease = updated.lease;
      action.reason = "reconciled after restart before service removal";
      releases.push_back(action);
      queued_releases_.insert(updated.lease);
      updated.lease = DrainLeaseId{};
    }

    if (updated.state != JobState::Blocked) {
      PassContext context;
      context.now = now;
      if (!transition_locked(updated, JobState::Blocked, BlockReason::ReconciledAfterRestart,
                             removed ? "controller restarted while the scope was out of service; "
                                       "verification and restoration are mandatory"
                                     : "controller restarted; prerequisites must be re-established",
                             now, context)) {
        continue;
      }
    } else {
      updated.block = BlockReason::ReconciledAfterRestart;
      updated.block_detail =
          "controller restarted; the previous block state was re-established conservatively";
      updated.last_detail = updated.block_detail;
    }
    ++stats_.reconciliations;
    updates.push_back(std::move(updated));
  }

  for (MaintenanceJob& job : updates) {
    const MaintenanceJob* stored = store_->state().jobs.find(job.id);
    if (stored == nullptr) {
      continue;
    }
    job.revision = stored->revision.next();
    job.updated_at = now;
  }
  if (!updates.empty()) {
    const Status committed = store_->commit_jobs(updates);
    if (!committed.ok()) {
      return committed;
    }
  }
  for (DrainAction& action : releases) {
    deferred_releases_.push_back(std::move(action));
  }
  return ok_status();
}

Result<JobSnapshot> Controller::status(JobId job) const {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  return current->snapshot();
}

std::vector<JobSnapshot> Controller::list() const {
  std::lock_guard<std::mutex> guard(mu_);
  std::vector<JobSnapshot> out;
  for (const auto& [id, job] : store_->state().jobs.jobs()) {
    (void)id;
    out.push_back(job.snapshot());
  }
  return out;
}

Result<Explanation> Controller::explain(JobId job, std::string action) const {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  const Nanos now = clock_->now();
  const ReadinessView readiness = ReadinessView::build(topology_, store_->state().evidence, policy_,
                                                       incarnation_, now);
  const Evaluation evaluation = evaluate_locked(*current, readiness, false, false, now);
  ExplainContext context;
  context.job = current;
  context.topology = &topology_;
  context.policy = &policy_;
  context.evidence = &store_->state().evidence;
  context.ledger = &store_->state().ledger;
  context.jobs = &store_->state().jobs;
  context.readiness = &readiness;
  context.preconditions = &evaluation.preconditions;
  context.impact = &evaluation.impact;
  context.current = incarnation_;
  context.now = now;
  context.action = std::move(action);
  return explain_job(context);
}

ConflictReport Controller::conflicts() const {
  std::lock_guard<std::mutex> guard(mu_);
  ConflictContext context;
  context.jobs = &store_->state().jobs;
  context.ledger = &store_->state().ledger;
  context.topology = &topology_;
  context.policy = &policy_;
  context.now = clock_->now();
  return inspect_conflicts(context);
}

Result<PreconditionSet> Controller::admission_check(JobId job) const {
  std::lock_guard<std::mutex> guard(mu_);
  const MaintenanceJob* current = store_->state().jobs.find(job);
  if (current == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  const Nanos now = clock_->now();
  const ReadinessView readiness = ReadinessView::build(topology_, store_->state().evidence, policy_,
                                                       incarnation_, now);
  return evaluate_locked(*current, readiness, false, false, now).preconditions;
}

ArbitrationReport Controller::last_arbitration() const {
  std::lock_guard<std::mutex> guard(mu_);
  return last_arbitration_;
}

std::vector<QuarantineRecord> Controller::quarantines() const {
  std::lock_guard<std::mutex> guard(mu_);
  std::vector<QuarantineRecord> out;
  for (const auto& [id, record] : store_->state().quarantines.records()) {
    (void)id;
    out.push_back(record);
  }
  return out;
}

ControllerStats Controller::stats() const {
  std::lock_guard<std::mutex> guard(mu_);
  return stats_;
}

Status Controller::shutdown() {
  std::lock_guard<std::mutex> guard(mu_);
  stopping_ = true;
  return store_->flush();
}

bool Controller::shutting_down() const {
  std::lock_guard<std::mutex> guard(mu_);
  return stopping_;
}

}  // namespace mf
