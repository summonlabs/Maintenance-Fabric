// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/impact.hpp"

#include <algorithm>
#include <set>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

std::vector<TargetId> sorted_unique(const std::vector<TargetId>& input) {
  std::vector<TargetId> out(input);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool contains(const std::vector<TargetId>& haystack, TargetId needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

std::optional<BlockReason> CapacityImpact::block_reason() const {
  for (const TargetImpact& entry : targets) {
    if (!entry.exists) {
      return BlockReason::TargetUnknown;
    }
  }
  for (const TargetImpact& entry : targets) {
    if (entry.planned && !entry.maintainable) {
      return BlockReason::TargetNotMaintainable;
    }
  }
  for (const PoolImpact& entry : pools) {
    if (!entry.satisfied) {
      return BlockReason::RedundancyViolation;
    }
  }
  for (const DomainImpact& entry : domains) {
    if (!entry.satisfied) {
      return BlockReason::FailureDomainLimit;
    }
  }
  for (const ContractImpact& entry : contracts) {
    if (!entry.satisfied) {
      return BlockReason::ContractViolation;
    }
  }
  return std::nullopt;
}

std::string CapacityImpact::first_violation() const {
  for (const PoolImpact& entry : pools) {
    if (!entry.satisfied) {
      return entry.reason;
    }
  }
  for (const DomainImpact& entry : domains) {
    if (!entry.satisfied) {
      return entry.reason;
    }
  }
  for (const ContractImpact& entry : contracts) {
    if (!entry.satisfied) {
      return entry.reason;
    }
  }
  for (const TargetImpact& entry : targets) {
    if (!entry.exists) {
      return "target " + to_string(entry.target) + " is not declared in the topology";
    }
    if (entry.planned && !entry.maintainable) {
      return "target " + to_string(entry.target) + " is marked not maintainable";
    }
  }
  return {};
}

std::string CapacityImpact::summary() const {
  std::string out = "planned=" + std::to_string(planned_count) +
                    " baseline=" + std::to_string(baseline_count) +
                    " pools=" + std::to_string(pools.size()) +
                    " domains=" + std::to_string(domains.size()) +
                    " contracts=" + std::to_string(contracts.size());
  out += satisfied ? " satisfied" : " UNSATISFIED";
  return out;
}

CapacityImpact compute_impact(const ImpactInput& input) {
  CapacityImpact impact;
  const Topology& topology = *input.topology;
  const Policy& policy = *input.policy;
  const ReadinessView& readiness = *input.readiness;

  const std::vector<TargetId> baseline = sorted_unique(input.baseline_removed);
  const std::vector<TargetId> planned = sorted_unique(input.planned_removed);
  impact.baseline_count = static_cast<std::uint32_t>(baseline.size());
  impact.planned_count = static_cast<std::uint32_t>(planned.size());

  // --- per-target facts -----------------------------------------------------
  std::vector<TargetId> all = baseline;
  for (const TargetId target : planned) {
    if (!contains(all, target)) {
      all.push_back(target);
    }
  }
  std::sort(all.begin(), all.end());
  for (const TargetId target : all) {
    const TargetRecord* record = topology.target(target);
    TargetImpact entry;
    entry.target = target;
    entry.exists = record != nullptr;
    entry.maintainable = record != nullptr && record->maintainable;
    entry.covered_by_baseline = contains(baseline, target);
    entry.planned = contains(planned, target);
    entry.evidence_fresh = readiness.is_fresh(target);
    entry.health = readiness.health_of(target);
    if (!entry.exists) {
      entry.detail = "not declared in the topology";
    } else if (!entry.evidence_fresh) {
      entry.detail = "no fresh health evidence";
    } else {
      entry.detail = std::string("health=") + to_string(entry.health);
    }
    impact.targets.push_back(std::move(entry));
  }
  for (const TargetImpact& entry : impact.targets) {
    if (!entry.exists) {
      impact.satisfied = false;
    }
    if (entry.planned && !entry.maintainable) {
      impact.satisfied = false;
    }
  }

  // --- pools ----------------------------------------------------------------
  std::vector<PoolId> pools;
  for (const TargetId target : all) {
    for (const PoolId pool : topology.pools_containing(target)) {
      if (std::find(pools.begin(), pools.end(), pool) == pools.end()) {
        pools.push_back(pool);
      }
    }
  }
  std::sort(pools.begin(), pools.end());
  for (const PoolId pool_id : pools) {
    const PoolRecord* pool = topology.pool(pool_id);
    if (pool == nullptr) {
      continue;
    }
    PoolImpact entry;
    entry.pool = pool_id;
    entry.name = pool->name;
    const RedundancyRule rule = policy.redundancy_for(pool_id);
    entry.required_units = rule.required_serving_units();
    entry.rule = rule.name.empty() ? "redundancy" : rule.name;
    if (const HeadroomRule* headroom = policy.headroom_for(pool_id); headroom != nullptr) {
      entry.reserve_units = headroom->reserve_units;
      if (!headroom->name.empty()) {
        entry.rule += "+" + headroom->name;
      }
    }

    std::uint32_t serving = 0;
    std::uint32_t unknown = 0;
    std::uint32_t removed_serving = 0;
    for (const TargetId member : pool->members) {
      const TargetRecord* record = topology.target(member);
      const std::uint32_t units = record == nullptr ? 1u : record->capacity_units;
      const bool out = contains(baseline, member) || contains(planned, member);
      entry.total_units += units;
      if (!readiness.is_fresh(member)) {
        unknown += units;
        continue;
      }
      if (!out && is_serving(readiness.health_of(member))) {
        serving += units;
      }
      if (out && is_serving(readiness.health_of(member))) {
        removed_serving += units;
      }
    }
    entry.serving_units = serving + removed_serving;
    entry.unknown_units = unknown;
    entry.removed_serving_units = removed_serving;
    entry.remaining_units = serving;

    const std::uint32_t required_total = entry.required_units + entry.reserve_units;
    if (serving < required_total) {
      entry.satisfied = false;
      entry.reason = "pool " + to_string(pool_id) + " would keep " + std::to_string(serving) +
                     " serving unit(s); policy requires " + std::to_string(required_total) +
                     " (" + std::to_string(entry.required_units) + " redundancy + " +
                     std::to_string(entry.reserve_units) + " reserve)";
      impact.satisfied = false;
    }
    if (input.require_full_evidence && unknown > 0) {
      // Reported for inspection: units without fresh evidence never count
      // toward the requirement, so the surviving count above is a proven lower
      // bound rather than an optimistic estimate.
      entry.reason += entry.reason.empty() ? "" : "; ";
      entry.reason += std::to_string(unknown) +
                      " unit(s) have no fresh health evidence and do not count toward the "
                      "requirement";
    }
    impact.pools.push_back(std::move(entry));
  }

  // --- correlated failure domains ------------------------------------------
  std::vector<DomainId> domains;
  for (const TargetId target : all) {
    const TargetRecord* record = topology.target(target);
    if (record == nullptr) {
      continue;
    }
    for (const DomainId domain : record->domains) {
      if (std::find(domains.begin(), domains.end(), domain) == domains.end()) {
        domains.push_back(domain);
      }
    }
  }
  std::sort(domains.begin(), domains.end());
  for (const DomainId domain_id : domains) {
    const DomainRecord* record = topology.domain(domain_id);
    if (record == nullptr) {
      continue;
    }
    DomainImpact entry;
    entry.domain = domain_id;
    entry.name = record->name;
    const std::vector<const DomainLimitRule*> rules = policy.domain_limits_for(domain_id);
    entry.limited = !rules.empty();
    std::uint32_t max_out = 0;
    for (const DomainLimitRule* rule : rules) {
      max_out = std::max(max_out, rule->max_out);
      if (!rule->name.empty()) {
        if (!entry.rule.empty()) {
          entry.rule += ",";
        }
        entry.rule += rule->name;
      }
    }
    if (entry.rule.empty()) {
      entry.rule = "domain-limit";
    }
    entry.max_out = max_out;

    std::uint32_t out_before = 0;
    std::uint32_t out_after = 0;
    for (const TargetId member : topology.domain_members(domain_id)) {
      const bool out_base = contains(baseline, member);
      const bool out_plan = contains(planned, member);
      if (out_base) {
        ++out_before;
      }
      if (out_base || out_plan) {
        ++out_after;
      }
    }
    entry.out_before = out_before;
    entry.out_after = out_after;
    if (entry.limited && out_after > max_out) {
      entry.satisfied = false;
      entry.reason = "correlated failure domain " + to_string(domain_id) + " would have " +
                     std::to_string(out_after) + " member(s) out of service; policy allows " +
                     std::to_string(max_out);
      impact.satisfied = false;
    }
    impact.domains.push_back(std::move(entry));
  }

  // --- availability contracts ----------------------------------------------
  const std::vector<const Contract*> contracts = policy.contracts_touching(planned);
  for (const Contract* contract : contracts) {
    ContractImpact entry;
    entry.contract = contract->id;
    entry.name = contract->name;
    entry.min_available = contract->min_available;
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    for (const TargetId member : contract->members) {
      const bool out_base = contains(baseline, member);
      const bool out_plan = contains(planned, member);
      const bool serving = readiness.is_fresh(member) && is_serving(readiness.health_of(member));
      if (serving && !out_base) {
        ++before;
      }
      if (serving && !out_base && !out_plan) {
        ++after;
      }
    }
    entry.available_before = before;
    entry.available_after = after;
    if (after < contract->min_available) {
      entry.satisfied = false;
      entry.reason = "contract " + to_string(contract->id) + " (" + contract->name +
                     ") would keep " + std::to_string(after) + " of " +
                     std::to_string(contract->members.size()) + " member(s) available; minimum is " +
                     std::to_string(contract->min_available);
      impact.satisfied = false;
    }
    impact.contracts.push_back(std::move(entry));
  }

  Digest digest;
  digest.update("mf.impact.v1");
  digest.update_u64(impact.planned_count);
  digest.update_u64(impact.baseline_count);
  digest.update_bool(impact.satisfied);
  for (const TargetImpact& entry : impact.targets) {
    digest.update_u32(static_cast<std::uint32_t>(entry.target.kind));
    digest.update_u32(entry.target.index);
    digest.update_bool(entry.exists);
    digest.update_bool(entry.maintainable);
    digest.update_bool(entry.covered_by_baseline);
    digest.update_bool(entry.planned);
    digest.update_bool(entry.evidence_fresh);
    digest.update_u32(static_cast<std::uint32_t>(entry.health));
  }
  for (const PoolImpact& entry : impact.pools) {
    digest.update_u64(entry.pool.value());
    digest.update_u32(entry.total_units);
    digest.update_u32(entry.serving_units);
    digest.update_u32(entry.unknown_units);
    digest.update_u32(entry.remaining_units);
    digest.update_u32(entry.required_units);
    digest.update_u32(entry.reserve_units);
    digest.update_bool(entry.satisfied);
  }
  for (const DomainImpact& entry : impact.domains) {
    digest.update_u32(static_cast<std::uint32_t>(entry.domain.kind));
    digest.update_u32(entry.domain.index);
    digest.update_u32(entry.out_before);
    digest.update_u32(entry.out_after);
    digest.update_u32(entry.max_out);
    digest.update_bool(entry.satisfied);
  }
  for (const ContractImpact& entry : impact.contracts) {
    digest.update_u64(entry.contract.value());
    digest.update_u32(entry.available_before);
    digest.update_u32(entry.available_after);
    digest.update_u32(entry.min_available);
    digest.update_bool(entry.satisfied);
  }
  impact.digest = digest.value();
  return impact;
}

}  // namespace mf
