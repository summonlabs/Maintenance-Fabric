// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/lifecycle.hpp"

#include <array>

namespace mf {
namespace {

struct StateName {
  JobState state;
  std::string_view name;
  std::string_view description;
};

constexpr std::array<StateName, kJobStateCount> kStateNames{{
    {JobState::Proposed, "proposed", "request recorded, not yet validated"},
    {JobState::Validated, "validated", "request shape and policy binding accepted"},
    {JobState::Prerequisites, "prerequisites", "authority, drain and evidence prerequisites pending"},
    {JobState::Ready, "ready", "all preconditions authoritatively satisfied and fresh"},
    {JobState::InMaintenance, "in-maintenance", "service removed, external maintenance running"},
    {JobState::Verifying, "verifying", "post-maintenance verification of the affected scope"},
    {JobState::Restoring, "restoring", "returning the scope to normal service"},
    {JobState::Complete, "complete", "restored and verified"},
    {JobState::Blocked, "blocked", "held by policy, evidence or an external failure"},
    {JobState::Cancelled, "cancelled", "withdrawn before any service was removed"},
    {JobState::Failed, "failed", "terminal failure; scope may still be quarantined"},
}};

}  // namespace

const char* to_string(JobState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.name.data();
    }
  }
  return "unknown";
}

const char* describe(JobState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.description.data();
    }
  }
  return "unknown";
}

std::optional<JobState> parse_job_state(std::string_view text) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.name == text) {
      return entry.state;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> stage_index(JobState state) noexcept {
  switch (state) {
    case JobState::Proposed: return 0;
    case JobState::Validated: return 1;
    case JobState::Prerequisites: return 2;
    case JobState::Ready: return 3;
    case JobState::InMaintenance: return 4;
    case JobState::Verifying: return 5;
    case JobState::Restoring: return 6;
    case JobState::Complete: return 7;
    case JobState::Blocked:
    case JobState::Cancelled:
    case JobState::Failed:
      return std::nullopt;
  }
  return std::nullopt;
}

TransitionCheck check_transition(JobState from, JobState to, bool service_removed) noexcept {
  if (from == to) {
    return TransitionCheck{false, "no-op transition"};
  }
  if (is_terminal(from)) {
    return TransitionCheck{false, "source state is terminal"};
  }
  if (to == JobState::Cancelled) {
    if (service_removed) {
      return TransitionCheck{false,
                             "service has been removed; the scope must be restored, not cancelled"};
    }
    return TransitionCheck{true, "withdrawn before service removal"};
  }
  if (to == JobState::Failed) {
    return TransitionCheck{true, "failure is legal from any non-terminal state"};
  }
  if (to == JobState::Blocked) {
    return TransitionCheck{true, "any non-terminal state may become blocked"};
  }

  const std::optional<std::uint32_t> from_stage = stage_index(from);
  const std::optional<std::uint32_t> to_stage = stage_index(to);

  if (from == JobState::Blocked) {
    if (!to_stage.has_value()) {
      return TransitionCheck{false, "blocked jobs may only resume onto the lifecycle chain"};
    }
    if (service_removed) {
      if (to == JobState::Verifying || to == JobState::Restoring) {
        return TransitionCheck{true, "resuming toward restoration"};
      }
      return TransitionCheck{
          false, "service was removed; a blocked job may only resume at verifying or restoring"};
    }
    if (to == JobState::Prerequisites || to == JobState::Ready) {
      return TransitionCheck{true, "resuming onto the lifecycle chain"};
    }
    return TransitionCheck{
        false,
        "a blocked job that removed no service must re-enter at prerequisites or ready, never at "
        "a removal or verification stage"};
  }

  if (from_stage.has_value() && to_stage.has_value()) {
    if (*to_stage == *from_stage + 1u) {
      return TransitionCheck{true, "advance to the next lifecycle stage"};
    }
    if (*to_stage < *from_stage) {
      if (service_removed) {
        return TransitionCheck{false,
                               "service has been removed; backward transitions are forbidden"};
      }
      if (*to_stage == 0) {
        return TransitionCheck{false, "a job never returns to proposed"};
      }
      return TransitionCheck{true, "backward transition permitted before service removal"};
    }
    return TransitionCheck{false, "lifecycle stages cannot be skipped"};
  }

  return TransitionCheck{false, "unsupported transition"};
}

std::vector<JobState> allowed_next_states(JobState from, bool service_removed) {
  std::vector<JobState> out;
  for (std::size_t i = 0; i < kJobStateCount; ++i) {
    const auto candidate = static_cast<JobState>(i);
    if (check_transition(from, candidate, service_removed).allowed) {
      out.push_back(candidate);
    }
  }
  return out;
}

}  // namespace mf
