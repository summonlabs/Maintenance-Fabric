// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/target.hpp"

#include <algorithm>

#include "mf/core/limits.hpp"

namespace mf {

const char* to_string(Health health) noexcept {
  switch (health) {
    case Health::Unknown: return "unknown";
    case Health::Healthy: return "healthy";
    case Health::Degraded: return "degraded";
    case Health::Down: return "down";
    case Health::Retired: return "retired";
  }
  return "unknown";
}

std::optional<Health> parse_health(std::string_view text) noexcept {
  if (text == "unknown") return Health::Unknown;
  if (text == "healthy") return Health::Healthy;
  if (text == "degraded") return Health::Degraded;
  if (text == "down") return Health::Down;
  if (text == "retired") return Health::Retired;
  return std::nullopt;
}

bool TargetRecord::in_domain(DomainId domain) const noexcept {
  return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

Status validate_target_record(const TargetRecord& record) {
  if (!record.id.valid()) {
    return invalid_argument("target id is not valid");
  }
  if (record.name.size() > limits::kMaxNameLength) {
    return make_error(ErrorCode::LimitExceeded, "target name exceeds maximum length");
  }
  if (record.domains.size() > limits::kMaxDomainsPerTarget) {
    return make_error(ErrorCode::LimitExceeded, "target belongs to too many failure domains");
  }
  if (record.capacity_units == 0) {
    return invalid_argument("target capacity_units must be greater than zero");
  }
  std::vector<DomainId> sorted(record.domains);
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return invalid_argument("target lists the same failure domain twice");
  }
  return ok_status();
}

}  // namespace mf
