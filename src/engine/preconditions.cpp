// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/preconditions.hpp"

#include <algorithm>
#include <array>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

struct KindName {
  PreconditionKind kind;
  std::string_view name;
};

constexpr std::array<KindName, kPreconditionKindCount> kKindNames{{
    {PreconditionKind::PolicyBound, "policy-bound"},
    {PreconditionKind::RequestShape, "request-shape"},
    {PreconditionKind::TargetsKnown, "targets-known"},
    {PreconditionKind::TargetsMaintainable, "targets-maintainable"},
    {PreconditionKind::DependencySatisfied, "dependency-satisfied"},
    {PreconditionKind::EvidenceFresh, "evidence-fresh"},
    {PreconditionKind::Redundancy, "redundancy"},
    {PreconditionKind::FailureDomain, "failure-domain"},
    {PreconditionKind::CapacityHeadroom, "capacity-headroom"},
    {PreconditionKind::ContractAvailability, "contract-availability"},
    {PreconditionKind::ConflictingMaintenance, "conflicting-maintenance"},
    {PreconditionKind::WindowAdmissible, "window-admissible"},
    {PreconditionKind::ControlPlaneQuorum, "control-plane-quorum"},
    {PreconditionKind::AuthorityReserved, "authority-reserved"},
    {PreconditionKind::DrainAcquired, "drain-acquired"},
}};

void add(PreconditionSet& set, PreconditionKind kind, PreconditionStatus status, BlockReason reason,
         std::string code, std::string detail) {
  PreconditionResult result;
  result.kind = kind;
  result.status = status;
  result.reason = reason;
  result.code = std::move(code);
  result.detail = std::move(detail);
  set.results.push_back(std::move(result));
}

bool contains(const std::vector<TargetId>& haystack, TargetId needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

bool intersects(const std::vector<TargetId>& a, const std::vector<TargetId>& b) {
  for (const TargetId value : a) {
    if (contains(b, value)) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* to_string(PreconditionKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

std::optional<PreconditionKind> parse_precondition_kind(std::string_view text) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.name == text) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

const char* to_string(PreconditionStatus status) noexcept {
  switch (status) {
    case PreconditionStatus::Satisfied: return "satisfied";
    case PreconditionStatus::Violated: return "violated";
    case PreconditionStatus::Pending: return "pending";
  }
  return "unknown";
}

std::optional<BlockReason> PreconditionSet::first_violation() const {
  for (const PreconditionResult& result : results) {
    if (result.status == PreconditionStatus::Violated) {
      return result.reason;
    }
  }
  return std::nullopt;
}

const PreconditionResult* PreconditionSet::find(PreconditionKind kind) const {
  for (const PreconditionResult& result : results) {
    if (result.kind == kind) {
      return &result;
    }
  }
  return nullptr;
}

std::string PreconditionSet::to_text() const {
  std::string out;
  for (const PreconditionResult& result : results) {
    out += "  [";
    out += to_string(result.status);
    out += "] ";
    out += to_string(result.kind);
    if (!result.detail.empty()) {
      out += " -- ";
      out += result.detail;
    }
    out.push_back('\n');
  }
  return out;
}

bool requires_operator_action(BlockReason reason) noexcept { return is_safety_block(reason); }

PreconditionSet evaluate_preconditions(const PreconditionContext& context) {
  PreconditionSet set;
  set.evaluated_at = context.now;
  set.evaluated_by = context.current;
  if (context.policy != nullptr) {
    set.policy_revision = context.policy->revision;
    set.policy_digest = context.policy->digest();
  }
  if (context.topology != nullptr) {
    set.topology_revision = context.topology->revision();
    set.topology_digest = context.topology->digest();
  }
  if (context.readiness != nullptr) {
    set.readiness_digest = context.readiness->digest();
  }

  const MaintenanceJob* job = context.job;
  if (job == nullptr || context.topology == nullptr || context.policy == nullptr ||
      context.readiness == nullptr) {
    add(set, PreconditionKind::RequestShape, PreconditionStatus::Violated, BlockReason::PolicyDenied,
        "missing-context", "precondition evaluation requires a job, topology, policy and readiness");
    set.violated = 1;
    set.all_satisfied = false;
    return set;
  }
  const Topology& topology = *context.topology;
  const Policy& policy = *context.policy;
  const ReadinessView& readiness = *context.readiness;

  // --- policy binding -------------------------------------------------------
  if (job->policy_revision.valid() && job->policy_revision != policy.revision) {
    add(set, PreconditionKind::PolicyBound, PreconditionStatus::Violated, BlockReason::PolicyDenied,
        "policy-revision-changed",
        "job was validated under policy revision " + to_string(job->policy_revision) +
            " but the current policy revision is " + to_string(policy.revision));
  } else {
    add(set, PreconditionKind::PolicyBound, PreconditionStatus::Satisfied, BlockReason::None,
        "policy-revision-current",
        "policy revision " + to_string(policy.revision) + ", topology revision " +
            to_string(topology.revision()));
  }

  // --- request shape --------------------------------------------------------
  {
    MaintenanceRequest request;
    request.title = job->title;
    request.reason = job->reason;
    request.targets = job->targets;
    request.depends_on = job->depends_on;
    request.window_filter = job->window_filter;
    request.priority = job->priority;
    request.estimated_duration = job->estimated_duration;
    request.requires_drain = job->requires_drain;
    request.requestor = job->requestor;
    const Status shape = validate_request_shape(request);
    if (shape.ok()) {
      add(set, PreconditionKind::RequestShape, PreconditionStatus::Satisfied, BlockReason::None,
          "request-shape-valid", "request shape is valid");
    } else {
      add(set, PreconditionKind::RequestShape, PreconditionStatus::Violated,
          BlockReason::PolicyDenied, "request-shape-invalid", format_error(shape.error()));
    }
  }

  // --- targets --------------------------------------------------------------
  {
    std::vector<TargetId> unknown;
    std::vector<TargetId> not_maintainable;
    for (const TargetId target : job->targets) {
      const TargetRecord* record = topology.target(target);
      if (record == nullptr) {
        unknown.push_back(target);
      } else if (!record->maintainable) {
        not_maintainable.push_back(target);
      }
    }
    if (unknown.empty()) {
      add(set, PreconditionKind::TargetsKnown, PreconditionStatus::Satisfied, BlockReason::None,
          "targets-known",
          std::to_string(job->targets.size()) + " requested target(s) exist; removal set is " +
              std::to_string(job->removal_set.size()) + " target(s)");
    } else {
      add(set, PreconditionKind::TargetsKnown, PreconditionStatus::Violated, BlockReason::TargetUnknown,
          "targets-unknown", "undeclared target(s): " + to_string(unknown.front()));
    }
    if (not_maintainable.empty()) {
      add(set, PreconditionKind::TargetsMaintainable, PreconditionStatus::Satisfied,
          BlockReason::None, "targets-maintainable", "every requested target is maintainable");
    } else {
      add(set, PreconditionKind::TargetsMaintainable, PreconditionStatus::Violated,
          BlockReason::TargetNotMaintainable, "target-not-maintainable",
          "target " + to_string(not_maintainable.front()) + " is marked not maintainable");
    }
  }

  // --- dependencies ---------------------------------------------------------
  {
    std::vector<std::string> unfinished;
    std::vector<std::string> broken;
    if (context.jobs != nullptr) {
      for (const JobId dependency : job->depends_on) {
        const MaintenanceJob* other = context.jobs->find(dependency);
        if (other == nullptr) {
          broken.push_back(to_string(dependency) + " (missing)");
        } else if (other->state == JobState::Complete) {
          continue;
        } else if (other->state == JobState::Cancelled || other->state == JobState::Failed) {
          broken.push_back(to_string(dependency) + " (" + to_string(other->state) + ")");
        } else {
          unfinished.push_back(to_string(dependency) + " (" + to_string(other->state) + ")");
        }
      }
    }
    if (!broken.empty()) {
      add(set, PreconditionKind::DependencySatisfied, PreconditionStatus::Violated,
          BlockReason::DependencyUnmet, "dependency-failed",
          "dependency " + broken.front() + " can never complete");
    } else if (!unfinished.empty()) {
      add(set, PreconditionKind::DependencySatisfied, PreconditionStatus::Pending,
          BlockReason::DependencyUnmet, "dependency-pending",
          "waiting for dependency " + unfinished.front());
    } else {
      add(set, PreconditionKind::DependencySatisfied, PreconditionStatus::Satisfied,
          BlockReason::None, "dependency-satisfied",
          std::to_string(job->depends_on.size()) + " dependency(ies) complete");
    }
  }

  // --- evidence freshness ---------------------------------------------------
  {
    const std::vector<TargetId> unfresh = readiness.unfresh(job->removal_set);
    if (unfresh.empty()) {
      add(set, PreconditionKind::EvidenceFresh, PreconditionStatus::Satisfied, BlockReason::None,
          "evidence-fresh",
          std::to_string(job->removal_set.size()) +
              " target(s) have fresh health evidence from at least " +
              std::to_string(policy.min_evidence_sources) + " source(s)");
    } else {
      const TargetReadiness* entry = readiness.get(unfresh.front());
      add(set, PreconditionKind::EvidenceFresh, PreconditionStatus::Violated,
          BlockReason::EvidenceMissingOrStale, "evidence-stale",
          "target " + to_string(unfresh.front()) + " lacks fresh evidence: " +
              (entry != nullptr ? entry->detail : std::string("no observation")) + " (" +
              std::to_string(unfresh.size()) + " of " +
              std::to_string(job->removal_set.size()) + " target(s) affected)");
    }
  }

  // --- capacity impact ------------------------------------------------------
  const CapacityImpact* impact = context.impact_in;
  CapacityImpact computed;
  if (impact == nullptr) {
    ImpactInput input;
    input.topology = &topology;
    input.policy = &policy;
    input.readiness = &readiness;
    input.planned_removed = job->removal_set;
    if (context.ledger != nullptr) {
      input.baseline_removed = context.ledger->reserved_targets(context.now);
    }
    if (context.jobs != nullptr) {
      for (const auto& [id, other] : context.jobs->jobs()) {
        // Only genuinely removed capacity forms the baseline; planning jobs are
        // represented by their reservations, and completed jobs have already
        // returned their scope to service.
        if (id == job->id || !stage_removes_service(other.state)) {
          continue;
        }
        for (const TargetId target : other.removal_set) {
          if (!contains(input.baseline_removed, target)) {
            input.baseline_removed.push_back(target);
          }
        }
      }
    }
    computed = compute_impact(input);
    impact = &computed;
  }
  set.impact_digest = impact->digest;
  if (context.impact_out != nullptr) {
    *context.impact_out = *impact;
  }

  {
    std::vector<std::string> failures;
    for (const PoolImpact& pool : impact->pools) {
      if (!pool.satisfied) {
        failures.push_back(pool.reason);
      }
    }
    if (failures.empty()) {
      add(set, PreconditionKind::Redundancy, PreconditionStatus::Satisfied, BlockReason::None,
          "redundancy-satisfied",
          std::to_string(impact->pools.size()) + " affected pool(s) keep their redundancy budget");
    } else {
      add(set, PreconditionKind::Redundancy, PreconditionStatus::Violated,
          BlockReason::RedundancyViolation, "redundancy-violated", failures.front());
    }
  }
  {
    std::vector<std::string> failures;
    for (const DomainImpact& domain : impact->domains) {
      if (!domain.satisfied) {
        failures.push_back(domain.reason);
      }
    }
    if (failures.empty()) {
      add(set, PreconditionKind::FailureDomain, PreconditionStatus::Satisfied, BlockReason::None,
          "failure-domain-satisfied",
          std::to_string(impact->domains.size()) + " affected failure domain(s) within limits");
    } else {
      add(set, PreconditionKind::FailureDomain, PreconditionStatus::Violated,
          BlockReason::FailureDomainLimit, "failure-domain-violated", failures.front());
    }
  }
  {
    std::vector<std::string> failures;
    for (const PoolImpact& pool : impact->pools) {
      if (pool.reserve_units > 0 && pool.remaining_units < pool.required_units + pool.reserve_units) {
        failures.push_back("pool " + to_string(pool.pool) + " would keep " +
                           std::to_string(pool.remaining_units) + " unit(s); policy reserves " +
                           std::to_string(pool.reserve_units) + " beyond the redundancy minimum");
      }
    }
    if (failures.empty()) {
      add(set, PreconditionKind::CapacityHeadroom, PreconditionStatus::Satisfied, BlockReason::None,
          "headroom-satisfied", "capacity reserve policy is preserved");
    } else {
      add(set, PreconditionKind::CapacityHeadroom, PreconditionStatus::Violated,
          BlockReason::CapacityHeadroom, "headroom-violated", failures.front());
    }
  }
  {
    std::vector<std::string> failures;
    for (const ContractImpact& contract : impact->contracts) {
      if (!contract.satisfied) {
        failures.push_back(contract.reason);
      }
    }
    if (failures.empty()) {
      add(set, PreconditionKind::ContractAvailability, PreconditionStatus::Satisfied,
          BlockReason::None, "contracts-satisfied",
          std::to_string(impact->contracts.size()) + " affected workload contract(s) satisfied");
    } else {
      add(set, PreconditionKind::ContractAvailability, PreconditionStatus::Violated,
          BlockReason::ContractViolation, "contract-violated", failures.front());
    }
  }

  // --- conflicting maintenance ---------------------------------------------
  {
    std::vector<std::string> conflicts;
    if (context.jobs != nullptr) {
      for (const auto& [id, other] : context.jobs->jobs()) {
        if (id == job->id) {
          continue;
        }
        // A job that is merely planning does not conflict: the arbitrator
        // decides between planners. Only capacity that is already committed
        // (service removed, or a live reservation) is a conflict.
        bool committed = stage_removes_service(other.state);
        if (context.ledger != nullptr) {
          const AuthorityReservation* held =
              context.ledger->find_for_job(other.id, other.generation);
          committed = committed || (held != nullptr && held->live_at(context.now));
        }
        if (!committed) {
          continue;
        }
        if (intersects(other.removal_set, job->removal_set)) {
          conflicts.push_back(to_string(id) + " already holds the same scope (" +
                              to_string(other.state) + ")");
        }
      }
    }
    if (context.ledger != nullptr) {
      for (const auto& [id, reservation] : context.ledger->reservations()) {
        if (reservation.job == job->id) {
          continue;
        }
        if (!reservation.live_at(context.now)) {
          continue;
        }
        if (intersects(reservation.targets, job->removal_set)) {
          const std::string text = "maintenance authority " + to_string(id) + " held by job " +
                                   to_string(reservation.job) + " covers the same scope";
          if (std::find(conflicts.begin(), conflicts.end(), text) == conflicts.end()) {
            conflicts.push_back(text);
          }
        }
      }
    }
    if (conflicts.empty()) {
      add(set, PreconditionKind::ConflictingMaintenance, PreconditionStatus::Satisfied,
          BlockReason::None, "no-conflict", "no other job or reservation claims this scope");
    } else {
      add(set, PreconditionKind::ConflictingMaintenance, PreconditionStatus::Violated,
          BlockReason::ConflictingMaintenance, "conflicting-maintenance", conflicts.front());
    }
  }

  // --- maintenance window ---------------------------------------------------
  {
    const WindowEvaluation evaluation =
        evaluate_window(policy.windows, job->targets, context.now, policy.require_window);
    if (evaluation.allows_start()) {
      add(set, PreconditionKind::WindowAdmissible, PreconditionStatus::Satisfied, BlockReason::None,
          "window-open", evaluation.detail);
    } else {
      add(set, PreconditionKind::WindowAdmissible, PreconditionStatus::Violated,
          BlockReason::WindowUnavailable, std::string("window-") + to_string(evaluation.verdict),
          evaluation.detail);
    }
  }

  // --- control-plane quorum -------------------------------------------------
  {
    bool touches_control_plane = false;
    for (const TargetId target : job->removal_set) {
      if (target.kind == TargetKind::ControlPlane) {
        touches_control_plane = true;
        break;
      }
    }
    if (!touches_control_plane) {
      add(set, PreconditionKind::ControlPlaneQuorum, PreconditionStatus::Satisfied, BlockReason::None,
          "control-plane-not-affected", "the removal set contains no control-plane component");
    } else if (context.evidence == nullptr) {
      add(set, PreconditionKind::ControlPlaneQuorum, PreconditionStatus::Pending,
          BlockReason::ControlPlaneUnsafe, "control-plane-evidence-unavailable",
          "no evidence store is available to prove control-plane quorum");
    } else {
      EvidenceQuery query;
      query.key.kind = EvidenceKind::ControlPlaneQuorum;
      query.key.subject = EvidenceSubject::global();
      query.key.source = SourceId::from_u64(1);
      query.current = context.current;
      query.now = context.now;
      bool proven = false;
      std::string detail = "no control-plane quorum observation";
      for (std::uint64_t source = 1; source <= limits::kMaxEvidenceSourcesPerKey; ++source) {
        query.key.source = SourceId::from_u64(source);
        const EvidenceRecord* record = context.evidence->find(query.key);
        if (record == nullptr) {
          continue;
        }
        const Freshness freshness = context.evidence->evaluate(query);
        if (freshness.fresh() && record->payload.result) {
          proven = true;
          detail = "control-plane quorum proven by " + to_string(query.key.source);
          break;
        }
        detail = "control-plane quorum observation is " + std::string(to_string(freshness.state));
      }
      if (proven) {
        add(set, PreconditionKind::ControlPlaneQuorum, PreconditionStatus::Satisfied,
            BlockReason::None, "control-plane-quorum-proven", detail);
      } else {
        add(set, PreconditionKind::ControlPlaneQuorum, PreconditionStatus::Violated,
            BlockReason::ControlPlaneUnsafe, "control-plane-quorum-unproven", detail);
      }
    }
  }

  // --- authority ------------------------------------------------------------
  if (context.check_authority) {
    if (context.ledger == nullptr) {
      add(set, PreconditionKind::AuthorityReserved, PreconditionStatus::Violated,
          BlockReason::AuthorityDenied, "authority-ledger-unavailable",
          "maintenance authority cannot be verified without a ledger");
    } else {
      const AuthorityReservation* reservation =
          context.ledger->find_for_job(job->id, job->generation);
      if (reservation == nullptr || !reservation->live_at(context.now)) {
        add(set, PreconditionKind::AuthorityReserved, PreconditionStatus::Pending,
            BlockReason::AuthorityDenied, "authority-not-held",
            "job does not currently hold live maintenance authority");
      } else if (reservation->attempt != job->active_attempt) {
        add(set, PreconditionKind::AuthorityReserved, PreconditionStatus::Violated,
            BlockReason::AuthorityDenied, "authority-attempt-mismatch",
            "authority " + to_string(reservation->id) + " belongs to attempt " +
                to_string(reservation->attempt) + " but the active attempt is " +
                to_string(job->active_attempt));
      } else {
        add(set, PreconditionKind::AuthorityReserved, PreconditionStatus::Satisfied,
            BlockReason::None, "authority-held",
            "authority " + to_string(reservation->id) + " covers " +
                std::to_string(reservation->targets.size()) + " target(s) until " +
                format_time(reservation->expires_at));
      }
    }
  } else {
    add(set, PreconditionKind::AuthorityReserved, PreconditionStatus::Pending,
        BlockReason::AuthorityDenied, "authority-not-requested",
        "authority is acquired during the prerequisites stage");
  }

  // --- drain ----------------------------------------------------------------
  if (context.check_drain && job->requires_drain) {
    if (!job->lease.valid()) {
      add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Pending, BlockReason::DrainFailed,
          "drain-not-held", "job does not hold a drain lease");
    } else if (context.evidence == nullptr) {
      add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Violated,
          BlockReason::DrainFailed, "drain-evidence-unavailable",
          "drain confirmation cannot be verified without an evidence store");
    } else {
      std::vector<TargetId> unconfirmed;
      for (const TargetId target : job->removal_set) {
        bool confirmed = false;
        for (std::uint64_t source = 1; source <= limits::kMaxEvidenceSourcesPerKey; ++source) {
          EvidenceQuery query;
          query.key.kind = EvidenceKind::DrainLeaseState;
          query.key.subject = EvidenceSubject::of(target);
          query.key.source = SourceId::from_u64(source);
          query.current = context.current;
          query.now = context.now;
          query.job = job->id;
          query.attempt = job->active_attempt;
          const EvidenceRecord* record = context.evidence->find(query.key);
          if (record == nullptr) {
            continue;
          }
          if (context.evidence->evaluate(query).fresh() && record->payload.result) {
            confirmed = true;
            break;
          }
        }
        if (!confirmed) {
          unconfirmed.push_back(target);
        }
      }
      if (unconfirmed.empty()) {
        add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Satisfied, BlockReason::None,
            "drain-confirmed", "drain lease " + to_string(job->lease) +
                                   " is confirmed for every target in the removal set");
      } else {
        add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Violated,
            BlockReason::DrainFailed, "drain-unconfirmed",
            "drain is not confirmed for target " + to_string(unconfirmed.front()) + " (" +
                std::to_string(unconfirmed.size()) + " target(s) unconfirmed)");
      }
    }
  } else if (context.check_drain) {
    add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Satisfied, BlockReason::None,
        "drain-not-required", "the request does not require a drain");
  } else {
    add(set, PreconditionKind::DrainAcquired, PreconditionStatus::Pending, BlockReason::DrainFailed,
        "drain-not-requested", "drain is acquired during the prerequisites stage");
  }

  for (const PreconditionResult& result : set.results) {
    switch (result.status) {
      case PreconditionStatus::Satisfied: ++set.satisfied; break;
      case PreconditionStatus::Violated: ++set.violated; break;
      case PreconditionStatus::Pending: ++set.pending; break;
    }
  }
  set.all_satisfied = set.violated == 0 && set.pending == 0;

  Digest digest;
  digest.update("mf.preconditions.v1");
  digest.update_u64(set.results.size());
  for (const PreconditionResult& result : set.results) {
    digest.update_u32(static_cast<std::uint32_t>(result.kind));
    digest.update_u32(static_cast<std::uint32_t>(result.status));
    digest.update_u32(static_cast<std::uint32_t>(result.reason));
    digest.update(result.code);
    digest.update(result.detail);
  }
  digest.update_u64(set.policy_revision.value());
  digest.update_u64(set.policy_digest);
  digest.update_u64(set.topology_revision.value());
  digest.update_u64(set.topology_digest);
  digest.update_u64(set.readiness_digest);
  digest.update_u64(set.impact_digest);
  digest.update(to_string(set.evaluated_by));
  set.digest = digest.value();
  return set;
}

}  // namespace mf
