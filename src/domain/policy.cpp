// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/policy.hpp"

#include <algorithm>
#include <set>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

RedundancyRule make_default_rule(PoolId pool) {
  RedundancyRule rule;
  rule.pool = pool;
  rule.min_viable = 1;
  rule.tolerated_losses = 0;
  rule.name = "conservative-default";
  return rule;
}

}  // namespace

RedundancyRule default_redundancy_rule(PoolId pool) { return make_default_rule(pool); }

RedundancyRule Policy::redundancy_for(PoolId pool) const {
  for (const RedundancyRule& rule : redundancy) {
    if (rule.pool == pool) {
      return rule;
    }
  }
  return default_redundancy_rule(pool);
}

const HeadroomRule* Policy::headroom_for(PoolId pool) const {
  for (const HeadroomRule& rule : headroom) {
    if (rule.pool == pool) {
      return &rule;
    }
  }
  return nullptr;
}

std::vector<const DomainLimitRule*> Policy::domain_limits_for(DomainId domain) const {
  std::vector<const DomainLimitRule*> out;
  for (const DomainLimitRule& rule : domain_limits) {
    if (rule.domain == domain) {
      out.push_back(&rule);
    }
  }
  return out;
}

bool Policy::has_domain_limit(DomainId domain) const noexcept {
  for (const DomainLimitRule& rule : domain_limits) {
    if (rule.domain == domain) {
      return true;
    }
  }
  return false;
}

const Contract* Policy::contract(ContractId id) const {
  for (const Contract& entry : contracts) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<const Contract*> Policy::contracts_touching(const std::vector<TargetId>& targets) const {
  std::vector<const Contract*> out;
  for (const Contract& entry : contracts) {
    for (const TargetId member : entry.members) {
      if (std::find(targets.begin(), targets.end(), member) != targets.end()) {
        out.push_back(&entry);
        break;
      }
    }
  }
  std::sort(out.begin(), out.end(),
            [](const Contract* a, const Contract* b) { return a->id < b->id; });
  return out;
}

Nanos Policy::ttl_for(EvidenceKind kind) const {
  const auto it = evidence_ttl.find(kind);
  if (it == evidence_ttl.end()) {
    return limits::kDefaultEvidenceTtlNanos;
  }
  return it->second;
}

bool Policy::has_ttl(EvidenceKind kind) const { return evidence_ttl.find(kind) != evidence_ttl.end(); }

std::uint32_t Policy::tier_rank(std::uint32_t tier_index) const {
  std::vector<std::uint32_t> indices;
  indices.reserve(tiers.size());
  for (const PriorityTier& tier : tiers) {
    indices.push_back(tier.index);
  }
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  for (std::size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] == tier_index) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return static_cast<std::uint32_t>(indices.size());
}

bool Policy::known_tier(std::uint32_t tier_index) const {
  for (const PriorityTier& tier : tiers) {
    if (tier.index == tier_index) {
      return true;
    }
  }
  return false;
}

const Window* Policy::window(WindowId id) const {
  for (const Window& entry : windows) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

Status Policy::validate() const {
  if (redundancy.size() > limits::kMaxRedundancyRules) {
    return make_error(ErrorCode::LimitExceeded, "too many redundancy rules");
  }
  if (domain_limits.size() > limits::kMaxDomainLimitRules) {
    return make_error(ErrorCode::LimitExceeded, "too many failure-domain limit rules");
  }
  if (contracts.size() > limits::kMaxContracts) {
    return make_error(ErrorCode::LimitExceeded, "too many availability contracts");
  }
  if (windows.size() > limits::kMaxWindowRules) {
    return make_error(ErrorCode::LimitExceeded, "too many maintenance windows");
  }
  if (tiers.size() > limits::kMaxPriorityTiers) {
    return make_error(ErrorCode::LimitExceeded, "too many priority tiers");
  }
  if (min_evidence_sources == 0 || min_evidence_sources > limits::kMaxEvidenceSourcesPerKey) {
    return invalid_argument("min_evidence_sources is out of range");
  }
  if (max_precondition_age <= 0 || max_precondition_age > limits::kMaxPreconditionAgeNanos) {
    return invalid_argument("max_precondition_age is out of range");
  }
  if (default_duration <= 0) {
    return invalid_argument("default_duration must be positive");
  }
  if (max_concurrent_jobs == 0 || max_concurrent_jobs > limits::kMaxLiveReservations) {
    return invalid_argument("max_concurrent_jobs is out of range");
  }
  if (authority_lease < limits::kMinDrainLeaseNanos || authority_lease > limits::kMaxAuthorityLeaseNanos) {
    return invalid_argument("authority_lease is out of range");
  }
  if (drain_lease < limits::kMinDrainLeaseNanos || drain_lease > limits::kMaxDrainLeaseNanos) {
    return invalid_argument("drain_lease is out of range");
  }
  if (max_drain_failures_before_block == 0 || max_verification_attempts == 0 ||
      max_restoration_attempts == 0) {
    return invalid_argument("retry bounds must be at least one");
  }
  if (!known_tier(0) && !tiers.empty()) {
    return invalid_argument("priority tier 0 must be declared");
  }
  for (const RedundancyRule& rule : redundancy) {
    if (!rule.pool.valid()) {
      return invalid_argument("redundancy rule references an invalid pool");
    }
    if (rule.min_viable == 0) {
      return invalid_argument("redundancy rule min_viable must be at least one");
    }
    if (rule.min_viable + rule.tolerated_losses < rule.min_viable) {
      return make_error(ErrorCode::Overflow, "redundancy rule requirement overflowed");
    }
  }
  for (const DomainLimitRule& rule : domain_limits) {
    if (!rule.domain.valid()) {
      return invalid_argument("failure-domain rule references an invalid domain");
    }
    if (rule.max_out == 0) {
      return invalid_argument("failure-domain rule max_out must be at least one");
    }
  }
  for (const Contract& entry : contracts) {
    if (!entry.id.valid()) {
      return invalid_argument("contract id is not valid");
    }
    if (entry.members.empty()) {
      return invalid_argument("contract " + to_string(entry.id) + " has no members");
    }
    if (entry.min_available == 0 || entry.min_available > entry.members.size()) {
      return invalid_argument("contract " + to_string(entry.id) + " min_available is out of range");
    }
    std::vector<TargetId> sorted(entry.members);
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      return invalid_argument("contract " + to_string(entry.id) + " repeats a member");
    }
  }
  for (const Window& window : windows) {
    if (!window.id.valid()) {
      return invalid_argument("window id is not valid");
    }
    if (window.closes_at <= window.opens_at) {
      return invalid_argument("window " + to_string(window.id) + " closes before it opens");
    }
    if (window.min_lead_time < 0 || window.abort_grace < 0) {
      return invalid_argument("window " + to_string(window.id) + " has a negative duration bound");
    }
    if (window.targets.size() > limits::kMaxWindowTargets) {
      return make_error(ErrorCode::LimitExceeded, "window target list is too large");
    }
  }
  for (const auto& [kind, ttl] : evidence_ttl) {
    if (ttl < limits::kMinEvidenceTtlNanos || ttl > limits::kMaxEvidenceTtlNanos) {
      return invalid_argument(std::string("evidence ttl for ") + to_string(kind) +
                              " is out of range");
    }
  }
  {
    std::set<std::uint32_t> seen;
    for (const PriorityTier& tier : tiers) {
      if (!seen.insert(tier.index).second) {
        return invalid_argument("priority tier " + std::to_string(tier.index) + " is declared twice");
      }
    }
  }
  return ok_status();
}

std::uint64_t Policy::digest() const {
  Digest digest;
  digest.update("mf.policy.v1");
  digest.update_u64(redundancy.size());
  for (const RedundancyRule& rule : redundancy) {
    digest.update_u64(rule.pool.value());
    digest.update_u32(rule.min_viable);
    digest.update_u32(rule.tolerated_losses);
    digest.update(rule.name);
  }
  digest.update_u64(domain_limits.size());
  for (const DomainLimitRule& rule : domain_limits) {
    digest.update_u32(static_cast<std::uint32_t>(rule.domain.kind));
    digest.update_u32(rule.domain.index);
    digest.update_u32(rule.max_out);
    digest.update(rule.name);
  }
  digest.update_u64(headroom.size());
  for (const HeadroomRule& rule : headroom) {
    digest.update_u64(rule.pool.value());
    digest.update_u32(rule.reserve_units);
    digest.update(rule.name);
  }
  digest.update_u64(contracts.size());
  for (const Contract& entry : contracts) {
    digest.update_u64(entry.id.value());
    digest.update(entry.name);
    digest.update_u64(entry.members.size());
    for (const TargetId member : entry.members) {
      digest.update_u32(static_cast<std::uint32_t>(member.kind));
      digest.update_u32(member.index);
    }
    digest.update_u32(entry.min_available);
    digest.update_bool(entry.require_fresh_evidence);
  }
  digest.update_u64(windows.size());
  for (const Window& window : windows) {
    digest.update_u64(window.id.value());
    digest.update(window.name);
    digest.update_i64(window.opens_at);
    digest.update_i64(window.closes_at);
    digest.update_i64(window.min_lead_time);
    digest.update_i64(window.abort_grace);
    digest.update_u32(static_cast<std::uint32_t>(window.on_close));
    digest.update_u64(window.targets.size());
    for (const TargetId target : window.targets) {
      digest.update_u32(static_cast<std::uint32_t>(target.kind));
      digest.update_u32(target.index);
    }
  }
  digest.update_u64(tiers.size());
  for (const PriorityTier& tier : tiers) {
    digest.update_u32(tier.index);
    digest.update(tier.name);
  }
  digest.update_u64(evidence_ttl.size());
  for (const auto& [kind, ttl] : evidence_ttl) {
    digest.update_u32(static_cast<std::uint32_t>(kind));
    digest.update_i64(ttl);
  }
  digest.update_u32(min_evidence_sources);
  digest.update_bool(require_window);
  digest.update_i64(max_precondition_age);
  digest.update_i64(default_duration);
  digest.update_u32(max_concurrent_jobs);
  digest.update_i64(authority_lease);
  digest.update_i64(drain_lease);
  digest.update_u32(max_drain_failures_before_block);
  digest.update_u32(max_verification_attempts);
  digest.update_u32(max_restoration_attempts);
  return digest.value();
}

}  // namespace mf
