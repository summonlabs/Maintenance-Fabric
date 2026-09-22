// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/scheduler.hpp"

#include <algorithm>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

void push(std::vector<JobId>& list, JobId id) { list.push_back(id); }

}  // namespace

Nanos renewal_margin(const Policy& policy) noexcept {
  const Nanos margin = policy.authority_lease / 4;
  return margin < kNanosPerSecond ? kNanosPerSecond : margin;
}

SchedulerSelection plan_tick(const SchedulerInputs& inputs) {
  SchedulerSelection selection;
  if (inputs.jobs == nullptr || inputs.policy == nullptr) {
    return selection;
  }
  selection.renewal_margin = renewal_margin(*inputs.policy);
  const Nanos now = inputs.now;

  for (const auto& [id, job] : inputs.jobs->jobs()) {
    (void)id;
    switch (job.state) {
      case JobState::Validated:
        push(selection.validated, job.id);
        push(selection.runnable, job.id);
        break;
      case JobState::Prerequisites:
      case JobState::Ready:
        push(selection.runnable, job.id);
        break;
      case JobState::Blocked:
        push(selection.blocked, job.id);
        push(selection.runnable, job.id);
        break;
      case JobState::InMaintenance:
        push(selection.in_maintenance, job.id);
        if (job.window_closes_at > 0 && now >= job.window_closes_at) {
          push(selection.overrun, job.id);
        }
        break;
      case JobState::Verifying:
        push(selection.verifying, job.id);
        break;
      case JobState::Restoring:
        push(selection.restoring, job.id);
        break;
      case JobState::Proposed:
      case JobState::Complete:
      case JobState::Cancelled:
      case JobState::Failed:
        break;
    }
    if (job.holds_resources()) {
      // The controller owns the lease expiry details; the scheduler only marks
      // jobs that could be approaching one so the tick always re-examines them.
      if (job.authority.valid() || job.lease.valid()) {
        push(selection.expiring, job.id);
      }
    }
  }

  const auto sort_ids = [](std::vector<JobId>& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  };
  sort_ids(selection.validated);
  sort_ids(selection.runnable);
  sort_ids(selection.in_maintenance);
  sort_ids(selection.verifying);
  sort_ids(selection.restoring);
  sort_ids(selection.overrun);
  sort_ids(selection.expiring);
  sort_ids(selection.blocked);

  Digest digest;
  digest.update("mf.scheduler.v1");
  digest.update_i64(now);
  const auto fold = [&digest](const char* tag, const std::vector<JobId>& list) {
    digest.update(tag);
    digest.update_u64(list.size());
    for (const JobId id : list) {
      digest.update_u64(id.value());
    }
  };
  fold("a", selection.validated);
  fold("r", selection.runnable);
  fold("m", selection.in_maintenance);
  fold("v", selection.verifying);
  fold("s", selection.restoring);
  fold("o", selection.overrun);
  fold("e", selection.expiring);
  fold("b", selection.blocked);
  selection.digest = digest.value();
  return selection;
}

}  // namespace mf
