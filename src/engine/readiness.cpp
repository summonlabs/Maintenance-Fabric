// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/readiness.hpp"

#include <algorithm>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

/// Worst-case ordering: an unknown or down member is never optimistically
/// treated as serving.
int health_severity(Health health) noexcept {
  switch (health) {
    case Health::Healthy: return 0;
    case Health::Degraded: return 1;
    case Health::Down: return 2;
    case Health::Retired: return 3;
    case Health::Unknown: return 4;
  }
  return 4;
}

}  // namespace

ReadinessView ReadinessView::build(const Topology& topology, const EvidenceStore& evidence,
                                   const Policy& policy, const ControllerIncarnation& current,
                                   Nanos now) {
  ReadinessView view;
  view.built_at_ = now;
  for (const auto& [id, record] : topology.targets()) {
    (void)record;
    TargetReadiness readiness;
    readiness.target = id;
    const EvidenceSubject subject = EvidenceSubject::of(id);
    const std::vector<SourceId> sources =
        evidence.fresh_sources(EvidenceKind::TargetHealth, subject, current, now);
    readiness.fresh_sources = sources.size();
    readiness.fresh = sources.size() >= static_cast<std::size_t>(policy.min_evidence_sources);
    if (sources.empty()) {
      readiness.state = FreshnessState::Missing;
      readiness.detail = "no fresh target-health evidence from the running incarnation";
      view.entries_.emplace(id, std::move(readiness));
      continue;
    }
    Health worst = Health::Healthy;
    bool first = true;
    for (const SourceId source : sources) {
      EvidenceKey key;
      key.kind = EvidenceKind::TargetHealth;
      key.subject = subject;
      key.source = source;
      const EvidenceRecord* observation = evidence.find(key);
      if (observation == nullptr) {
        continue;
      }
      if (first || health_severity(observation->payload.health) > health_severity(worst)) {
        worst = observation->payload.health;
      }
      if (first) {
        readiness.source = source;
        readiness.observed_at = observation->observed_at;
        first = false;
      } else if (source < readiness.source) {
        readiness.source = source;
      }
    }
    readiness.health = worst;
    readiness.state = FreshnessState::Fresh;
    readiness.detail = readiness.fresh
                           ? "fresh target-health evidence from " +
                                 std::to_string(sources.size()) + " source(s)"
                           : "target-health evidence from fewer sources than policy requires";
    view.entries_.emplace(id, std::move(readiness));
  }
  return view;
}

const TargetReadiness* ReadinessView::get(TargetId id) const noexcept {
  const auto it = entries_.find(id);
  return it == entries_.end() ? nullptr : &it->second;
}

Health ReadinessView::health_of(TargetId id) const noexcept {
  const TargetReadiness* entry = get(id);
  return entry == nullptr ? Health::Unknown : entry->health;
}

bool ReadinessView::is_fresh(TargetId id) const noexcept {
  const TargetReadiness* entry = get(id);
  return entry != nullptr && entry->fresh;
}

bool ReadinessView::is_serving(TargetId id) const noexcept {
  const TargetReadiness* entry = get(id);
  if (entry == nullptr || !entry->fresh) {
    return false;
  }
  return mf::is_serving(entry->health);
}

std::vector<TargetId> ReadinessView::unfresh(const std::vector<TargetId>& scope) const {
  std::vector<TargetId> out;
  for (const TargetId target : scope) {
    if (!is_fresh(target)) {
      out.push_back(target);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::uint64_t ReadinessView::digest() const {
  Digest digest;
  digest.update("mf.readiness.v1");
  digest.update_i64(built_at_);
  digest.update_u64(entries_.size());
  for (const auto& [id, entry] : entries_) {
    digest.update_u32(static_cast<std::uint32_t>(id.kind));
    digest.update_u32(id.index);
    digest.update_u32(static_cast<std::uint32_t>(entry.health));
    digest.update_bool(entry.fresh);
    digest.update_u64(entry.fresh_sources);
    digest.update_i64(entry.observed_at);
  }
  return digest.value();
}

}  // namespace mf
