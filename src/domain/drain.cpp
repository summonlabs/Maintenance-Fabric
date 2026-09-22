// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/drain.hpp"

#include <algorithm>

namespace mf {

const char* to_string(DrainState state) noexcept {
  switch (state) {
    case DrainState::Unknown: return "unknown";
    case DrainState::Draining: return "draining";
    case DrainState::Drained: return "drained";
    case DrainState::Failed: return "failed";
    case DrainState::Released: return "released";
    case DrainState::NotFound: return "not-found";
  }
  return "unknown";
}

std::optional<DrainState> parse_drain_state(std::string_view text) noexcept {
  if (text == "unknown") return DrainState::Unknown;
  if (text == "draining") return DrainState::Draining;
  if (text == "drained") return DrainState::Drained;
  if (text == "failed") return DrainState::Failed;
  if (text == "released") return DrainState::Released;
  if (text == "not-found") return DrainState::NotFound;
  return std::nullopt;
}

DrainPort::~DrainPort() = default;

Status validate_drain_request(const DrainRequest& request) {
  if (!request.job.valid() || !request.generation.valid() || !request.attempt.valid()) {
    return invalid_argument("drain request is missing job, generation or attempt identity");
  }
  if (!request.requester.valid()) {
    return invalid_argument("drain request is missing the requesting incarnation");
  }
  if (request.targets.empty()) {
    return invalid_argument("drain request has no targets");
  }
  if (request.targets.size() > limits::kMaxDrainTargetsPerRequest) {
    return make_error(ErrorCode::LimitExceeded, "drain request exceeds the target limit");
  }
  if (request.lease_duration < limits::kMinDrainLeaseNanos ||
      request.lease_duration > limits::kMaxDrainLeaseNanos) {
    return invalid_argument("drain lease duration is out of range");
  }
  std::vector<TargetId> sorted(request.targets);
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return invalid_argument("drain request repeats a target");
  }
  for (const TargetId target : request.targets) {
    if (!target.valid()) {
      return invalid_argument("drain request contains an invalid target id");
    }
  }
  return ok_status();
}

}  // namespace mf
