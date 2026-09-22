// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/window.hpp"

#include <algorithm>
#include <cstddef>

namespace mf {

const char* to_string(OnWindowClose policy) noexcept {
  switch (policy) {
    case OnWindowClose::FinishActiveStep: return "finish-active-step";
    case OnWindowClose::ExtendToComplete: return "extend-to-complete";
    case OnWindowClose::AbortAndRestore: return "abort-and-restore";
  }
  return "unknown";
}

std::optional<OnWindowClose> parse_on_window_close(std::string_view text) noexcept {
  if (text == "finish-active-step" || text == "finish") return OnWindowClose::FinishActiveStep;
  if (text == "extend-to-complete" || text == "extend") return OnWindowClose::ExtendToComplete;
  if (text == "abort-and-restore" || text == "abort") return OnWindowClose::AbortAndRestore;
  return std::nullopt;
}

const char* to_string(WindowVerdict verdict) noexcept {
  switch (verdict) {
    case WindowVerdict::NoWindowConfigured: return "no-window-configured";
    case WindowVerdict::Open: return "open";
    case WindowVerdict::NotYetOpen: return "not-yet-open";
    case WindowVerdict::TooLateToStart: return "too-late-to-start";
    case WindowVerdict::Closed: return "closed";
    case WindowVerdict::NoMatchingWindow: return "no-matching-window";
  }
  return "unknown";
}

bool Window::applies_to(TargetId target) const noexcept {
  if (targets.empty()) {
    return true;
  }
  return std::find(targets.begin(), targets.end(), target) != targets.end();
}

bool window_closed_after(const Window& window, Nanos now) noexcept {
  return now >= window.closes_at;
}

WindowEvaluation evaluate_window(const std::vector<Window>& windows,
                                 const std::vector<TargetId>& targets, Nanos now,
                                 bool require_window) {
  std::vector<const Window*> applicable;
  for (const Window& window : windows) {
    bool matches = targets.empty();
    for (const TargetId target : targets) {
      if (window.applies_to(target)) {
        matches = true;
        break;
      }
    }
    if (matches) {
      applicable.push_back(&window);
    }
  }
  std::sort(applicable.begin(), applicable.end(),
            [](const Window* a, const Window* b) { return a->id < b->id; });

  if (applicable.empty()) {
    WindowEvaluation evaluation;
    if (require_window) {
      evaluation.verdict = WindowVerdict::NoMatchingWindow;
      evaluation.detail = "policy requires a maintenance window and none matches the job targets";
    } else {
      evaluation.verdict = WindowVerdict::NoWindowConfigured;
      evaluation.detail = "no maintenance window configured; admission is not window gated";
    }
    return evaluation;
  }

  const auto classify = [now](const Window& window) {
    if (now < window.opens_at) {
      return WindowVerdict::NotYetOpen;
    }
    if (now >= window.closes_at) {
      return WindowVerdict::Closed;
    }
    if (window.closes_at - now < window.min_lead_time) {
      return WindowVerdict::TooLateToStart;
    }
    return WindowVerdict::Open;
  };

  // applicable is ordered by window id, so the first match in each category is
  // the deterministic choice.
  const Window* best_open = nullptr;
  for (const Window* window : applicable) {
    if (classify(*window) == WindowVerdict::Open) {
      best_open = window;
      break;
    }
  }
  const Window* chosen = best_open != nullptr ? best_open : applicable.front();
  const WindowVerdict verdict = best_open != nullptr ? WindowVerdict::Open : classify(*chosen);

  WindowEvaluation evaluation;
  evaluation.verdict = verdict;
  evaluation.window = chosen->id;
  evaluation.closes_at = chosen->closes_at;
  evaluation.remaining = chosen->closes_at - now;
  switch (verdict) {
    case WindowVerdict::Open:
      evaluation.detail = "window " + to_string(chosen->id) + " is open with lead time satisfied";
      break;
    case WindowVerdict::NotYetOpen:
      evaluation.detail = "window " + to_string(chosen->id) + " has not opened yet";
      break;
    case WindowVerdict::TooLateToStart:
      evaluation.detail = "window " + to_string(chosen->id) +
                          " closes sooner than the required lead time";
      break;
    case WindowVerdict::Closed:
      evaluation.detail = "window " + to_string(chosen->id) + " is closed";
      break;
    case WindowVerdict::NoWindowConfigured:
    case WindowVerdict::NoMatchingWindow:
      break;
  }
  return evaluation;
}

}  // namespace mf
