#pragma once

// Time is always supplied by an injected clock. The runtime never calls the
// wall clock directly, so every decision and every freshness evaluation is
// reproducible under a manual clock in tests.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mf {

using Nanos = std::int64_t;

inline constexpr Nanos kNanosPerMicrosecond = 1000LL;
inline constexpr Nanos kNanosPerMillisecond = 1000000LL;
inline constexpr Nanos kNanosPerSecond = 1000000000LL;
inline constexpr Nanos kNanosPerMinute = 60LL * kNanosPerSecond;
inline constexpr Nanos kNanosPerHour = 60LL * kNanosPerMinute;

class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  [[nodiscard]] virtual Nanos now() const noexcept = 0;
};

/// Wall-clock source used by the daemon and the CLI.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] Nanos now() const noexcept override;
};

/// Deterministic clock for tests and for replaying recorded decisions.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Nanos start = 1750000000000000000LL) noexcept : now_(start) {}
  [[nodiscard]] Nanos now() const noexcept override { return now_.load(std::memory_order_relaxed); }
  void set(Nanos value) noexcept { now_.store(value, std::memory_order_relaxed); }
  void advance(Nanos delta) noexcept { now_.fetch_add(delta, std::memory_order_relaxed); }

 private:
  std::atomic<Nanos> now_;
};

/// RFC 3339 UTC rendering with nanosecond precision: 2026-01-01T00:00:00.000000000Z
[[nodiscard]] std::string format_time(Nanos value);
/// Accepts RFC 3339 UTC ("...Z"), "epoch:<seconds>", or a relative offset such
/// as "-30s" / "+5m" interpreted against the supplied reference instant.
[[nodiscard]] std::optional<Nanos> parse_time(std::string_view text, Nanos reference);
/// Compact human duration: 1h2m3.500s / 12.500ms / 900ns
[[nodiscard]] std::string format_duration(Nanos value);

}  // namespace mf
