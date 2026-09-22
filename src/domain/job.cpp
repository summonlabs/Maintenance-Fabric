// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/job.hpp"

#include <algorithm>
#include <array>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

struct BlockName {
  BlockReason reason;
  std::string_view name;
};

constexpr std::array<BlockName, kBlockReasonCount> kBlockNames{{
    {BlockReason::None, "none"},
    {BlockReason::TargetUnknown, "target-unknown"},
    {BlockReason::TargetNotMaintainable, "target-not-maintainable"},
    {BlockReason::EvidenceMissingOrStale, "evidence-missing-or-stale"},
    {BlockReason::RedundancyViolation, "redundancy-violation"},
    {BlockReason::FailureDomainLimit, "failure-domain-limit"},
    {BlockReason::ContractViolation, "contract-violation"},
    {BlockReason::CapacityHeadroom, "capacity-headroom"},
    {BlockReason::ConflictingMaintenance, "conflicting-maintenance"},
    {BlockReason::DependencyUnmet, "dependency-unmet"},
    {BlockReason::WindowUnavailable, "window-unavailable"},
    {BlockReason::AuthorityDenied, "authority-denied"},
    {BlockReason::DrainFailed, "drain-failed"},
    {BlockReason::DrainLeaseLost, "drain-lease-lost"},
    {BlockReason::AwaitingCompletionReport, "awaiting-completion-report"},
    {BlockReason::VerificationFailed, "verification-failed"},
    {BlockReason::RestorationFailed, "restoration-failed"},
    {BlockReason::ReconciledAfterRestart, "reconciled-after-restart"},
    {BlockReason::ControlPlaneUnsafe, "control-plane-unsafe"},
    {BlockReason::PolicyDenied, "policy-denied"},
    {BlockReason::OperatorHold, "operator-hold"},
    {BlockReason::Overflow, "overflow"},
}};

}  // namespace

const char* to_string(BlockReason reason) noexcept {
  for (const BlockName& entry : kBlockNames) {
    if (entry.reason == reason) {
      return entry.name.data();
    }
  }
  return "unknown";
}

std::optional<BlockReason> parse_block_reason(std::string_view text) noexcept {
  for (const BlockName& entry : kBlockNames) {
    if (entry.name == text) {
      return entry.reason;
    }
  }
  return std::nullopt;
}

bool is_safety_block(BlockReason reason) noexcept {
  switch (reason) {
    case BlockReason::EvidenceMissingOrStale:
    case BlockReason::RedundancyViolation:
    case BlockReason::FailureDomainLimit:
    case BlockReason::ContractViolation:
    case BlockReason::CapacityHeadroom:
    case BlockReason::ConflictingMaintenance:
    case BlockReason::DrainLeaseLost:
    case BlockReason::ControlPlaneUnsafe:
    case BlockReason::TargetNotMaintainable:
    case BlockReason::TargetUnknown:
      return true;
    default:
      return false;
  }
}

const char* to_string(AttemptOutcome outcome) noexcept {
  switch (outcome) {
    case AttemptOutcome::Running: return "running";
    case AttemptOutcome::Succeeded: return "succeeded";
    case AttemptOutcome::Failed: return "failed";
    case AttemptOutcome::Fenced: return "fenced";
    case AttemptOutcome::Cancelled: return "cancelled";
  }
  return "unknown";
}

std::optional<AttemptOutcome> parse_attempt_outcome(std::string_view text) noexcept {
  if (text == "running") return AttemptOutcome::Running;
  if (text == "succeeded") return AttemptOutcome::Succeeded;
  if (text == "failed") return AttemptOutcome::Failed;
  if (text == "fenced") return AttemptOutcome::Fenced;
  if (text == "cancelled") return AttemptOutcome::Cancelled;
  return std::nullopt;
}

std::optional<AttemptRecord> MaintenanceJob::active_attempt_record() const {
  if (!active_attempt.valid()) {
    return std::nullopt;
  }
  for (const AttemptRecord& record : attempts) {
    if (record.id == active_attempt) {
      return record;
    }
  }
  return std::nullopt;
}

void MaintenanceJob::push_history(HistoryEntry entry) {
  history.push_back(std::move(entry));
  if (history.size() > limits::kMaxJobHistoryEntries) {
    const std::size_t excess = history.size() - limits::kMaxJobHistoryEntries;
    history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(excess));
    history_dropped += static_cast<std::uint32_t>(excess);
  }
}

std::uint64_t MaintenanceJob::digest() const {
  Digest digest;
  digest.update("mf.job.v1");
  digest.update_u64(id.value());
  digest.update_u64(generation.value());
  digest.update(title);
  digest.update(reason);
  digest.update_u64(targets.size());
  for (const TargetId target : targets) {
    digest.update_u32(static_cast<std::uint32_t>(target.kind));
    digest.update_u32(target.index);
  }
  digest.update_u64(removal_set.size());
  for (const TargetId target : removal_set) {
    digest.update_u32(static_cast<std::uint32_t>(target.kind));
    digest.update_u32(target.index);
  }
  digest.update_u64(depends_on.size());
  for (const JobId dependency : depends_on) {
    digest.update_u64(dependency.value());
  }
  digest.update_u32(priority);
  digest.update_i64(estimated_duration);
  digest.update_bool(requires_drain);
  digest.update_u64(requestor.value());
  digest.update_u32(static_cast<std::uint32_t>(state));
  digest.update_u32(static_cast<std::uint32_t>(block));
  digest.update_bool(service_removed);
  digest.update_bool(overrun);
  digest.update_bool(approved);
  digest.update_u64(authority.value());
  digest.update_u64(lease.value());
  digest.update(to_string(active_attempt));
  digest.update_u64(next_attempt.value());
  digest.update_i64(created_at);
  digest.update_i64(updated_at);
  digest.update_i64(started_at);
  digest.update_i64(completed_at);
  digest.update_u64(preconditions_digest);
  digest.update_i64(preconditions_at);
  digest.update_bool(preconditions_satisfied);
  digest.update_u32(verification_passes);
  digest.update_u32(verification_failures);
  digest.update_u32(restoration_failures);
  digest.update_u32(drain_failures);
  digest.update_i64(verification_started_at);
  digest.update_i64(restoration_started_at);
  digest.update_bool(maintenance_failed);
  digest.update_bool(window_extended);
  return digest.value();
}

JobSnapshot MaintenanceJob::snapshot() const {
  JobSnapshot snapshot;
  snapshot.id = id;
  snapshot.generation = generation;
  snapshot.revision = revision;
  snapshot.state = state;
  snapshot.block = block;
  snapshot.block_detail = block_detail;
  snapshot.targets = targets;
  snapshot.priority = priority;
  snapshot.service_removed = service_removed;
  snapshot.overrun = overrun;
  snapshot.authority = authority;
  snapshot.lease = lease;
  snapshot.active_attempt = active_attempt;
  snapshot.created_at = created_at;
  snapshot.updated_at = updated_at;
  return snapshot;
}

std::string MaintenanceJob::to_text() const {
  std::string out = to_string(id);
  out += " gen=";
  out += to_string(generation);
  out += " state=";
  out += to_string(state);
  if (block != BlockReason::None) {
    out += " block=";
    out += to_string(block);
  }
  out += " targets=[";
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += to_string(targets[i]);
  }
  out.push_back(']');
  out += " authority=";
  out += to_string(authority);
  out += " lease=";
  out += to_string(lease);
  return out;
}

Status JobTable::insert(MaintenanceJob job) {
  if (!job.id.valid()) {
    return invalid_argument("job id is not valid");
  }
  if (jobs_.size() >= limits::kMaxJobsTotal && jobs_.find(job.id) == jobs_.end()) {
    return make_error(ErrorCode::LimitExceeded, "job table is at capacity");
  }
  if (jobs_.find(job.id) != jobs_.end()) {
    return make_error(ErrorCode::AlreadyExists, "job " + to_string(job.id) + " already exists");
  }
  jobs_.emplace(job.id, std::move(job));
  return ok_status();
}

Status JobTable::put(MaintenanceJob job) {
  const auto it = jobs_.find(job.id);
  if (it == jobs_.end()) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job.id) + " does not exist");
  }
  if (job.revision != it->second.revision.next()) {
    return make_error(ErrorCode::StaleRevision,
                      "replacement for job " + to_string(job.id) + " is at revision " +
                          to_string(job.revision) + " but the stored revision is " +
                          to_string(it->second.revision));
  }
  it->second = std::move(job);
  return ok_status();
}

Status JobTable::adopt(MaintenanceJob job) {
  if (!job.id.valid()) {
    return invalid_argument("adopted job has no identity");
  }
  jobs_[job.id] = std::move(job);
  return ok_status();
}

Status JobTable::erase(JobId id) {
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) {
    return make_error(ErrorCode::NotFound, "job " + to_string(id) + " does not exist");
  }
  jobs_.erase(it);
  return ok_status();
}

MaintenanceJob* JobTable::find(JobId id) noexcept {
  const auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}

const MaintenanceJob* JobTable::find(JobId id) const noexcept {
  const auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}

JobId JobTable::allocate_id() {
  const JobId id = next_id_;
  next_id_ = next_id_.next();
  return id;
}

std::uint64_t JobTable::digest() const {
  Digest digest;
  digest.update("mf.jobtable.v1");
  digest.update_u64(jobs_.size());
  for (const auto& [id, job] : jobs_) {
    digest.update_u64(id.value());
    digest.update_u64(job.digest());
  }
  return digest.value();
}

void JobTable::clear() {
  jobs_.clear();
  next_id_ = JobId::from_u64(1);
}

std::uint64_t quarantine_digest(const QuarantineRecord& record) {
  Digest digest;
  digest.update("mf.quarantine.v1");
  digest.update_u64(record.id.value());
  digest.update_u32(static_cast<std::uint32_t>(record.target.kind));
  digest.update_u32(record.target.index);
  digest.update_u64(record.job.value());
  digest.update_u64(record.generation.value());
  digest.update_u32(static_cast<std::uint32_t>(record.reason));
  digest.update(record.detail);
  digest.update_i64(record.created_at);
  digest.update(to_string(record.created_by));
  digest.update_bool(record.cleared);
  digest.update_i64(record.cleared_at);
  return digest.value();
}

Status QuarantineTable::insert(QuarantineRecord record) {
  if (!record.id.valid() || !record.target.valid()) {
    return invalid_argument("quarantine record is not well formed");
  }
  if (records_.find(record.id) != records_.end()) {
    return make_error(ErrorCode::AlreadyExists,
                      "quarantine " + to_string(record.id) + " already exists");
  }
  if (record.digest == 0) {
    record.digest = quarantine_digest(record);
  }
  records_.emplace(record.id, std::move(record));
  return ok_status();
}

Status QuarantineTable::adopt(QuarantineRecord record) {
  if (!record.id.valid() || !record.target.valid()) {
    return invalid_argument("persisted quarantine record is not well formed");
  }
  if (record.digest == 0) {
    record.digest = quarantine_digest(record);
  }
  if (record.id >= next_id_) {
    next_id_ = record.id.next();
  }
  records_[record.id] = std::move(record);
  return ok_status();
}

Status QuarantineTable::put(QuarantineRecord record) {
  if (records_.find(record.id) == records_.end()) {
    return make_error(ErrorCode::NotFound,
                      "quarantine " + to_string(record.id) + " does not exist");
  }
  record.digest = quarantine_digest(record);
  records_[record.id] = std::move(record);
  return ok_status();
}

Status QuarantineTable::erase(QuarantineId id) {
  if (records_.erase(id) == 0) {
    return make_error(ErrorCode::NotFound, "quarantine " + to_string(id) + " does not exist");
  }
  return ok_status();
}

const QuarantineRecord* QuarantineTable::find(QuarantineId id) const noexcept {
  const auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}

const QuarantineRecord* QuarantineTable::find_open_for_target(TargetId target) const noexcept {
  const QuarantineRecord* best = nullptr;
  for (const auto& [id, record] : records_) {
    (void)id;
    if (record.cleared || record.target != target) {
      continue;
    }
    if (best == nullptr || record.created_at > best->created_at ||
        (record.created_at == best->created_at && record.id < best->id)) {
      best = &record;
    }
  }
  return best;
}

QuarantineId QuarantineTable::allocate_id() {
  const QuarantineId id = next_id_;
  next_id_ = next_id_.next();
  return id;
}

void QuarantineTable::clear() {
  records_.clear();
  next_id_ = QuarantineId::from_u64(1);
}

Status validate_request_shape(const MaintenanceRequest& request) {
  if (request.title.empty() || request.title.size() > limits::kMaxTitleLength) {
    return invalid_argument("request title is empty or too long");
  }
  if (request.reason.size() > limits::kMaxReasonLength) {
    return make_error(ErrorCode::LimitExceeded, "request reason exceeds the maximum length");
  }
  if (request.targets.empty()) {
    return invalid_argument("request must name at least one target");
  }
  if (request.targets.size() > limits::kMaxTargetsPerJob) {
    return make_error(ErrorCode::LimitExceeded, "request exceeds the maximum target count");
  }
  if (request.depends_on.size() > limits::kMaxDependenciesPerJob) {
    return make_error(ErrorCode::LimitExceeded, "request exceeds the maximum dependency count");
  }
  std::vector<TargetId> sorted(request.targets);
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return invalid_argument("request repeats a target");
  }
  for (const TargetId target : request.targets) {
    if (!target.valid()) {
      return invalid_argument("request contains an invalid target id");
    }
  }
  std::vector<JobId> deps(request.depends_on);
  std::sort(deps.begin(), deps.end());
  if (std::adjacent_find(deps.begin(), deps.end()) != deps.end()) {
    return invalid_argument("request repeats a dependency");
  }
  if (request.estimated_duration < 0) {
    return invalid_argument("request estimated_duration must not be negative");
  }
  if (!request.requestor.valid()) {
    return invalid_argument("request must name a requestor");
  }
  return ok_status();
}

}  // namespace mf
