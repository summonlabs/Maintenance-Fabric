#pragma once

// The maintenance lifecycle is a strict state machine. Backward movement is
// only legal while no service has been removed: once a resource is out of
// service the job may only move toward verification and restoration (or fail),
// never back to an earlier planning stage.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mf {

enum class JobState : std::uint8_t {
  Proposed = 0,
  Validated = 1,
  Prerequisites = 2,
  Ready = 3,
  InMaintenance = 4,
  Verifying = 5,
  Restoring = 6,
  Complete = 7,
  Blocked = 8,
  Cancelled = 9,
  Failed = 10,
};

inline constexpr std::size_t kJobStateCount = 11;

[[nodiscard]] const char* to_string(JobState state) noexcept;
[[nodiscard]] std::optional<JobState> parse_job_state(std::string_view text) noexcept;
[[nodiscard]] const char* describe(JobState state) noexcept;

/// Position on the linear chain proposed -> ... -> complete. Blocked, Cancelled
/// and Failed are off-chain and return nullopt.
[[nodiscard]] std::optional<std::uint32_t> stage_index(JobState state) noexcept;

[[nodiscard]] constexpr bool is_terminal(JobState state) noexcept {
  return state == JobState::Complete || state == JobState::Cancelled || state == JobState::Failed;
}

/// True when the job holds, or may hold, durable resources (authority
/// reservation, drain lease) that must be retired before the job can be closed.
[[nodiscard]] constexpr bool holds_resources(JobState state) noexcept {
  return state == JobState::Prerequisites || state == JobState::Ready ||
         state == JobState::InMaintenance || state == JobState::Verifying ||
         state == JobState::Restoring || state == JobState::Blocked;
}

/// True for the stages in which the target is understood to be out of service.
[[nodiscard]] constexpr bool stage_removes_service(JobState state) noexcept {
  return state == JobState::InMaintenance || state == JobState::Verifying ||
         state == JobState::Restoring;
}

/// Outcome of p to becoming the successor of p from.
struct TransitionCheck {
  bool allowed{false};
  const char* reason{"unspecified"};
};

/// p service_removed is the job's sticky flag: it becomes true when the job
/// first enters in-maintenance and never returns to false.
[[nodiscard]] TransitionCheck check_transition(JobState from, JobState to,
                                               bool service_removed) noexcept;

/// Deterministic, ascending list of legal successors. Used by the CLI and by
/// the explanation renderer.
[[nodiscard]] std::vector<JobState> allowed_next_states(JobState from, bool service_removed);

}  // namespace mf
