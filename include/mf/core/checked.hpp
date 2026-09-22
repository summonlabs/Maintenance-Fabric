#pragma once

// Checked arithmetic. Every value derived from external input (files, frames,
// CLI arguments, counters) that participates in a size or capacity computation
// passes through one of these helpers. Overflow yields nullopt, never a wrap.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace mf {

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_add(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_add requires an unsigned type");
  if (a > static_cast<T>(static_cast<T>(~static_cast<T>(0)) - b)) {
    return std::nullopt;
  }
  return static_cast<T>(a + b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_sub requires an unsigned type");
  if (b > a) {
    return std::nullopt;
  }
  return static_cast<T>(a - b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_mul requires an unsigned type");
  if (a == 0 || b == 0) {
    return static_cast<T>(0);
  }
  if (a > static_cast<T>(static_cast<T>(~static_cast<T>(0)) / b)) {
    return std::nullopt;
  }
  return static_cast<T>(a * b);
}

/// Signed multiplication with an explicit pre-check; the product is only
/// formed once it is known to fit.
template <class T>
[[nodiscard]] constexpr std::optional<T> checked_mul_signed(T a, T b) noexcept {
  static_assert(std::is_signed_v<T>, "checked_mul_signed requires a signed type");
  constexpr T kMax = std::numeric_limits<T>::max();
  constexpr T kMin = std::numeric_limits<T>::lowest();
  if (a == 0 || b == 0) {
    return static_cast<T>(0);
  }
  if (a > 0) {
    if (b > 0) {
      if (a > static_cast<T>(kMax / b)) return std::nullopt;
    } else {
      if (b < static_cast<T>(kMin / a)) return std::nullopt;
    }
  } else {
    if (b > 0) {
      if (a < static_cast<T>(kMin / b)) return std::nullopt;
    } else {
      if (a < static_cast<T>(kMax / b)) return std::nullopt;
    }
  }
  return static_cast<T>(a * b);
}

/// Narrowing conversion that fails instead of truncating.
template <class To, class From>
[[nodiscard]] constexpr std::optional<To> narrow(From value) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>,
                "narrow requires integral types");
  if (!std::in_range<To>(value)) {
    return std::nullopt;
  }
  return static_cast<To>(value);
}

template <class To, class From>
[[nodiscard]] constexpr To narrow_or(From value, To fallback) noexcept {
  const std::optional<To> r = narrow<To>(value);
  return r.has_value() ? *r : fallback;
}

/// Saturating conversion for counters that must not abort a hot path.
template <class To, class From>
[[nodiscard]] constexpr To saturate(From value) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>,
                "saturate requires integral types");
  if (std::in_range<To>(value)) {
    return static_cast<To>(value);
  }
  return value < From{0} ? std::numeric_limits<To>::lowest() : std::numeric_limits<To>::max();
}

}  // namespace mf
