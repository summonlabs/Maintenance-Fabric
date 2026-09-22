#pragma once

// Maintenance windows are policy constraints, never timers that silently kill
// running work. A window gates admission (may a job start?) and records overrun
// policy. It can never prevent a target from being returned to service: the
// restoring and verifying stages are always admissible.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mf/core/time.hpp"
#include "mf/domain/identity.hpp"

namespace mf {

/// What the policy asks for when a window closes while an attempt is running.
/// Maintenance Fabric does not perform the maintenance action itself, so it can
/// never "abort" a vendor firmware operation; it can only stop granting further
/// authority and drive the job to verification and restoration.
enum class OnWindowClose : std::uint8_t {
  /// Let the running attempt finish, then verify and restore. Recorded as an overrun.
  FinishActiveStep = 0,
  /// Same, but the policy explicitly sanctions the overrun: no overrun is recorded.
  ExtendToComplete = 1,
  /// Stop granting further authority immediately and require the running attempt
  /// to report completion within abort_grace; otherwise the job fails and the
  /// target stays quarantined.
  AbortAndRestore = 2,
};

[[nodiscard]] const char* to_string(OnWindowClose policy) noexcept;
[[nodiscard]] std::optional<OnWindowClose> parse_on_window_close(std::string_view text) noexcept;

struct Window {
  WindowId id;
  std::string name;
  Nanos opens_at{0};
  Nanos closes_at{0};
  /// A start is only admitted while closes_at - now >= min_lead_time.
  Nanos min_lead_time{0};
  /// Grace granted to a running attempt under AbortAndRestore.
  Nanos abort_grace{0};
  OnWindowClose on_close{OnWindowClose::FinishActiveStep};
  /// Empty means the window applies to every target.
  std::vector<TargetId> targets;

  [[nodiscard]] bool applies_to(TargetId target) const noexcept;
  [[nodiscard]] bool covers(Nanos instant) const noexcept {
    return instant >= opens_at && instant < closes_at;
  }
};

enum class WindowVerdict : std::uint8_t {
  /// No window is configured for the job and policy does not require one.
  NoWindowConfigured = 0,
  Open = 1,
  NotYetOpen = 2,
  /// A window applies but closes sooner than min_lead_time from now.
  TooLateToStart = 3,
  Closed = 4,
  /// Policy requires a window and none matches the job's targets.
  NoMatchingWindow = 5,
};

[[nodiscard]] const char* to_string(WindowVerdict verdict) noexcept;

struct WindowEvaluation {
  WindowVerdict verdict{WindowVerdict::NoWindowConfigured};
  WindowId window;
  Nanos closes_at{0};
  Nanos remaining{0};
  std::string detail;

  [[nodiscard]] bool allows_start() const noexcept {
    return verdict == WindowVerdict::Open || verdict == WindowVerdict::NoWindowConfigured;
  }
};

/// Deterministic evaluation. When several windows apply, the one with the
/// smallest WindowId that allows a start wins; otherwise the smallest WindowId
/// that is applicable at all determines the reported verdict.
[[nodiscard]] WindowEvaluation evaluate_window(const std::vector<Window>& windows,
                                               const std::vector<TargetId>& targets, Nanos now,
                                               bool require_window);

/// True when p window has closed at p now while the job was still active.
[[nodiscard]] bool window_closed_after(const Window& window, Nanos now) noexcept;

}  // namespace mf
