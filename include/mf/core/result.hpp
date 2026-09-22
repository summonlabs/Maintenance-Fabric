#pragma once

// Explicit success/failure values. The runtime never throws for expected
// conditions and never uses exceptions to signal policy denial.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace mf {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  LimitExceeded,
  Overflow,
  StaleGeneration,
  StaleIncarnation,
  StaleRevision,
  StaleEvidence,
  StaleAttempt,
  StaleAuthority,
  PolicyDenied,
  StateConflict,
  WindowClosed,
  DependencyUnmet,
  EvidenceMissing,
  DrainFailed,
  DrainLeaseLost,
  RestorationFailed,
  Unauthorized,
  Busy,
  Cancelled,
  ShuttingDown,
  Timeout,
  IoError,
  CorruptState,
  VersionMismatch,
  Unsupported,
  Internal,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

/// True when the condition is a fence rejection: the caller raced with a newer
/// generation/incarnation/revision and must not retry with the same token.
[[nodiscard]] constexpr bool is_fence_error(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleIncarnation:
    case ErrorCode::StaleRevision:
    case ErrorCode::StaleAttempt:
    case ErrorCode::StaleAuthority:
      return true;
    default:
      return false;
  }
}

struct Error {
  ErrorCode code{ErrorCode::Ok};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
};

[[nodiscard]] Error make_error(ErrorCode code, std::string detail);
[[nodiscard]] std::string format_error(const Error& error);

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& noexcept { return *value_; }
  [[nodiscard]] T& value() & noexcept { return *value_; }
  [[nodiscard]] T&& value() && noexcept { return std::move(*value_); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  std::optional<T> value_;
  Error error_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status ok_status() noexcept { return Status{}; }

[[nodiscard]] inline Error internal_error(std::string detail) {
  return make_error(ErrorCode::Internal, std::move(detail));
}
[[nodiscard]] inline Error invalid_argument(std::string detail) {
  return make_error(ErrorCode::InvalidArgument, std::move(detail));
}
[[nodiscard]] inline Error not_found(std::string detail) {
  return make_error(ErrorCode::NotFound, std::move(detail));
}

}  // namespace mf
