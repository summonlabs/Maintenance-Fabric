// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/arbitrator.hpp"

#include <algorithm>
#include <limits>

#include "mf/core/hash.hpp"

namespace mf {

const char* to_string(ArbitrationOutcome outcome) noexcept {
  switch (outcome) {
    case ArbitrationOutcome::Granted: return "granted";
    case ArbitrationOutcome::Denied: return "denied";
    case ArbitrationOutcome::Deferred: return "deferred";
  }
  return "unknown";
}

std::vector<ArbitrationCandidate> order_candidates(const std::vector<const MaintenanceJob*>& jobs,
                                                   const Policy& policy) {
  std::vector<ArbitrationCandidate> out;
  out.reserve(jobs.size());
  for (const MaintenanceJob* job : jobs) {
    if (job == nullptr) {
      continue;
    }
    ArbitrationCandidate candidate;
    candidate.job = job->id;
    candidate.tier = job->priority;
    candidate.tier_rank = policy.tier_rank(job->priority);
    candidate.created_at = job->created_at;
    candidate.ordering_key = "tier=" + std::to_string(job->priority) +
                             " rank=" + std::to_string(candidate.tier_rank) +
                             " created=" + std::to_string(job->created_at) +
                             " job=" + to_string(job->id);
    out.push_back(std::move(candidate));
  }
  std::sort(out.begin(), out.end(),
            [](const ArbitrationCandidate& a, const ArbitrationCandidate& b) {
              if (a.tier_rank != b.tier_rank) {
                return a.tier_rank < b.tier_rank;
              }
              if (a.created_at != b.created_at) {
                return a.created_at < b.created_at;
              }
              return a.job < b.job;
            });
  return out;
}

std::string ArbitrationReport::to_text() const {
  std::string out = "arbitration at ";
  out += format_time(at);
  out += " by ";
  out += to_string(by);
  out += "\n";
  for (const ArbitrationDecision& decision : decisions) {
    out += "  ";
    out += to_string(decision.job);
    out += " ";
    out += to_string(decision.outcome);
    out += " [";
    out += decision.ordering_key;
    out += "]";
    if (!decision.detail.empty()) {
      out += " -- ";
      out += decision.detail;
    }
    out.push_back('\n');
  }
  return out;
}

ArbitrationReport arbitrate(const ArbitrationContext& context) {
  ArbitrationReport report;
  report.at = context.now;
  report.by = context.current;
  if (context.policy != nullptr) {
    report.policy_revision = context.policy->revision;
    report.policy_digest = context.policy->digest();
  }
  if (context.topology != nullptr) {
    report.topology_revision = context.topology->revision();
    report.topology_digest = context.topology->digest();
  }
  if (context.jobs == nullptr || context.policy == nullptr || context.topology == nullptr ||
      context.readiness == nullptr || context.ledger == nullptr) {
    report.digest = 0;
    return report;
  }

  std::vector<const MaintenanceJob*> selected;
  selected.reserve(context.candidates.size());
  for (const JobId id : context.candidates) {
    const MaintenanceJob* job = context.jobs->find(id);
    if (job != nullptr) {
      selected.push_back(job);
    }
  }
  const std::vector<ArbitrationCandidate> ordered = order_candidates(selected, *context.policy);

  std::size_t position = 0;
  for (const ArbitrationCandidate& candidate : ordered) {
    ArbitrationDecision decision;
    decision.job = candidate.job;
    decision.position = position++;
    decision.tier_rank = candidate.tier_rank;
    decision.created_at = candidate.created_at;
    decision.ordering_key = candidate.ordering_key;

    const MaintenanceJob* job = context.jobs->find(candidate.job);
    if (job == nullptr) {
      decision.outcome = ArbitrationOutcome::Denied;
      decision.reason = BlockReason::TargetUnknown;
      decision.detail = "job disappeared during arbitration";
      report.decisions.push_back(std::move(decision));
      ++report.denied;
      continue;
    }
    if (job->active_attempt.valid()) {
      const AuthorityReservation* existing =
          context.ledger->find_for_job(job->id, job->generation);
      if (existing != nullptr && existing->live_at(context.now) &&
          existing->attempt == job->active_attempt) {
        decision.outcome = ArbitrationOutcome::Deferred;
        decision.reason = BlockReason::None;
        decision.authority = existing->id;
        decision.detail = "job already holds live maintenance authority " + to_string(existing->id);
        report.decisions.push_back(std::move(decision));
        ++report.deferred;
        continue;
      }
    }
    if (report.granted >= context.policy->max_concurrent_jobs) {
      decision.outcome = ArbitrationOutcome::Denied;
      decision.reason = BlockReason::AuthorityDenied;
      decision.detail =
          "concurrency limit reached: policy allows " +
          std::to_string(context.policy->max_concurrent_jobs) + " concurrent maintenance job(s)";
      report.decisions.push_back(std::move(decision));
      ++report.denied;
      continue;
    }

    AuthorityAcquireContext acquire;
    acquire.topology = context.topology;
    acquire.policy = context.policy;
    acquire.readiness = context.readiness;
    acquire.ledger = context.ledger;
    acquire.job = job->id;
    acquire.generation = job->generation;
    acquire.attempt = job->active_attempt;
    acquire.removal_set = job->removal_set;
    acquire.current = context.current;
    acquire.now = context.now;
    CapacityImpact impact;
    acquire.impact_out = &impact;

    Result<AuthorityId> granted = acquire_authority(acquire);
    decision.impact_digest = impact.digest;
    if (!granted.ok()) {
      decision.outcome = ArbitrationOutcome::Denied;
      if (granted.error().code == ErrorCode::StaleEvidence) {
        decision.reason = BlockReason::EvidenceMissingOrStale;
      } else if (granted.error().detail.find("already covers target") != std::string::npos) {
        decision.reason = BlockReason::ConflictingMaintenance;
      } else {
        decision.reason = impact.block_reason().value_or(BlockReason::AuthorityDenied);
      }
      decision.detail = granted.error().detail;
      report.decisions.push_back(std::move(decision));
      ++report.denied;
      continue;
    }
    decision.outcome = ArbitrationOutcome::Granted;
    decision.authority = granted.value();
    decision.detail = "granted maintenance authority " + to_string(granted.value()) + " with " +
                      impact.summary();
    if (context.impact_out != nullptr && report.granted == 0) {
      *context.impact_out = impact;
    }
    report.decisions.push_back(std::move(decision));
    ++report.granted;
  }

  Digest digest;
  digest.update("mf.arbitration.v1");
  digest.update_i64(report.at);
  digest.update(to_string(report.by));
  digest.update_u64(report.policy_revision.value());
  digest.update_u64(report.policy_digest);
  digest.update_u64(report.topology_revision.value());
  digest.update_u64(report.topology_digest);
  for (const ArbitrationDecision& decision : report.decisions) {
    digest.update_u64(decision.job.value());
    digest.update_u32(static_cast<std::uint32_t>(decision.outcome));
    digest.update_u64(decision.position);
    digest.update_u32(decision.tier_rank);
    digest.update_i64(decision.created_at);
    digest.update(decision.ordering_key);
    digest.update_u32(static_cast<std::uint32_t>(decision.reason));
    digest.update(decision.detail);
    digest.update_u64(decision.authority.value());
  }
  report.digest = digest.value();
  return report;
}

}  // namespace mf
