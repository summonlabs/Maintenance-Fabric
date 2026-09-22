// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/authority.hpp"

#include <algorithm>

namespace mf {

Result<AuthorityId> acquire_authority(const AuthorityAcquireContext& context) {
  if (context.topology == nullptr || context.policy == nullptr || context.readiness == nullptr ||
      context.ledger == nullptr) {
    return internal_error("authority acquisition requires topology, policy, readiness and ledger");
  }
  if (!context.job.valid() || !context.generation.valid() || !context.attempt.valid()) {
    return invalid_argument("authority acquisition requires job, generation and attempt identity");
  }
  if (context.removal_set.empty()) {
    return invalid_argument("authority acquisition requires a non-empty removal set");
  }
  const Policy& policy = *context.policy;

  // Evidence first: without a fresh view of the targets themselves, no capacity
  // conclusion is meaningful.
  const std::vector<TargetId> unfresh = context.readiness->unfresh(context.removal_set);
  if (!unfresh.empty()) {
    return make_error(ErrorCode::StaleEvidence,
                      "no fresh health evidence for target " + to_string(unfresh.front()) + " (" +
                          std::to_string(unfresh.size()) + " of " +
                          std::to_string(context.removal_set.size()) +
                          " target(s) in the removal set)");
  }

  // Two jobs may never hold the same target at once. Without this rule the
  // capacity maths alone would happily grant both, because a target present in
  // the baseline and in the plan is still only one unit out of service.
  for (const auto& [id, reservation] : context.ledger->reservations()) {
    if (!reservation.live_at(context.now)) {
      continue;
    }
    if (reservation.job == context.job && reservation.generation == context.generation) {
      continue;
    }
    for (const TargetId target : context.removal_set) {
      if (reservation.covers(target)) {
        return make_error(ErrorCode::PolicyDenied,
                          "maintenance authority " + to_string(id) + " held by job " +
                              to_string(reservation.job) + " already covers target " +
                              to_string(target));
      }
    }
  }

  Nanos lease = context.lease_duration > 0 ? context.lease_duration : policy.authority_lease;
  lease = std::clamp(lease, limits::kMinDrainLeaseNanos, limits::kMaxAuthorityLeaseNanos);

  ImpactInput input;
  input.topology = context.topology;
  input.policy = &policy;
  input.readiness = context.readiness;
  input.planned_removed = context.removal_set;
  input.baseline_removed = context.ledger->reserved_targets(context.now);
  const CapacityImpact impact = compute_impact(input);
  if (context.impact_out != nullptr) {
    *context.impact_out = impact;
  }
  if (!impact.satisfied) {
    const std::optional<BlockReason> reason = impact.block_reason();
    return make_error(ErrorCode::PolicyDenied,
                      std::string("removal would violate ") +
                          to_string(reason.value_or(BlockReason::PolicyDenied)) + ": " +
                          impact.first_violation());
  }

  AuthorityReservation reservation;
  reservation.job = context.job;
  reservation.generation = context.generation;
  reservation.attempt = context.attempt;
  reservation.owner = context.current;
  reservation.targets = context.removal_set;
  std::sort(reservation.targets.begin(), reservation.targets.end());
  reservation.targets.erase(std::unique(reservation.targets.begin(), reservation.targets.end()),
                            reservation.targets.end());
  reservation.domains = context.topology->domains_of(reservation.targets);
  reservation.pools = context.topology->pools_of(reservation.targets);
  reservation.granted_at = context.now;
  reservation.expires_at = context.now + lease;

  Result<AuthorityId> granted = context.ledger->reserve(std::move(reservation));
  if (!granted.ok()) {
    return granted.error();
  }
  return granted.value();
}

}  // namespace mf
