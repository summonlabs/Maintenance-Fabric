#pragma once

// Pure, deterministic tick planning. The scheduler never mutates state and never
// invokes a callback: it classifies the current job table into the work lists a
// tick would process, in a reproducible order. The controller performs the
// imperative driving, so no callback is ever invoked beneath a lock.

#include <cstdint>
#include <string>
#include <vector>

#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"

namespace mf {

struct SchedulerInputs {
  const JobTable* jobs{nullptr};
  const Policy* policy{nullptr};
  Nanos now{0};
};

struct SchedulerSelection {
  /// Approved jobs that still need their scope expanded and an attempt created.
  std::vector<JobId> validated;
  /// Jobs that may move toward readiness, ascending by job id.
  std::vector<JobId> runnable;
  /// Jobs whose external maintenance action is running.
  std::vector<JobId> in_maintenance;
  std::vector<JobId> verifying;
  std::vector<JobId> restoring;
  /// In-maintenance jobs whose window has already closed.
  std::vector<JobId> overrun;
  /// Jobs holding authority or a drain lease that expires within the renewal
  /// margin and must be renewed or retired this tick.
  std::vector<JobId> expiring;
  std::vector<JobId> blocked;
  Nanos renewal_margin{0};
  std::uint64_t digest{0};
};

[[nodiscard]] SchedulerSelection plan_tick(const SchedulerInputs& inputs);

/// Margin before expiry at which a lease is renewed.
[[nodiscard]] Nanos renewal_margin(const Policy& policy) noexcept;

}  // namespace mf
