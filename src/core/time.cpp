// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/time.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "mf/core/checked.hpp"
#include "mf/core/id.hpp"

namespace mf {
namespace {

constexpr std::int64_t kSecondsPerDay = 86400;

struct Civil {
  int year;
  unsigned month;
  unsigned day;
};

/// Days-to-civil conversion (proleptic Gregorian).
Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
  const int year = static_cast<int>(y + (m <= 2u ? 1 : 0));
  return Civil{year, m, d};
}

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2u ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool all_digits(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

std::optional<std::uint32_t> fixed_uint(std::string_view text) noexcept {
  if (!all_digits(text)) {
    return std::nullopt;
  }
  return parse_u32(text);
}

}  // namespace

Clock::~Clock() = default;

Nanos SystemClock::now() const noexcept {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since);
  return static_cast<Nanos>(nanos.count());
}

std::string format_time(Nanos value) {
  std::int64_t seconds = value / kNanosPerSecond;
  std::int64_t fraction = value % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }
  std::int64_t days = seconds / kSecondsPerDay;
  std::int64_t second_of_day = seconds % kSecondsPerDay;
  if (second_of_day < 0) {
    second_of_day += kSecondsPerDay;
    days -= 1;
  }
  const Civil civil = civil_from_days(days);
  const unsigned hour = static_cast<unsigned>(second_of_day / 3600);
  const unsigned minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const unsigned second = static_cast<unsigned>(second_of_day % 60);

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02u.%09lldZ", civil.year,
                civil.month, civil.day, hour, minute, second,
                static_cast<long long>(fraction));
  return std::string(buffer);
}

std::optional<Nanos> parse_time(std::string_view text, Nanos reference) {
  if (text.empty()) {
    return std::nullopt;
  }
  // Relative offset: +5m / -30s / -2h / +250ms / +900ns
  if (text.front() == '+' || text.front() == '-') {
    const bool negative = text.front() == '-';
    std::string_view body = text.substr(1);
    std::size_t digits = 0;
    while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') {
      ++digits;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    const std::optional<std::uint64_t> magnitude = parse_u64(body.substr(0, digits));
    if (!magnitude.has_value()) {
      return std::nullopt;
    }
    const std::string_view unit = body.substr(digits);
    std::int64_t scale = 0;
    if (unit == "s" || unit.empty()) {
      scale = kNanosPerSecond;
    } else if (unit == "m") {
      scale = kNanosPerMinute;
    } else if (unit == "h") {
      scale = kNanosPerHour;
    } else if (unit == "ms") {
      scale = kNanosPerMillisecond;
    } else if (unit == "us") {
      scale = kNanosPerMicrosecond;
    } else if (unit == "ns") {
      scale = 1;
    } else {
      return std::nullopt;
    }
    const std::optional<std::int64_t> delta = narrow<std::int64_t>(*magnitude);
    if (!delta.has_value()) {
      return std::nullopt;
    }
    return negative ? reference - (*delta * scale) : reference + (*delta * scale);
  }
  // Explicit epoch seconds: epoch:1750000000
  if (text.substr(0, 6) == "epoch:") {
    const std::optional<std::uint64_t> secs = parse_u64(text.substr(6));
    if (!secs.has_value()) {
      return std::nullopt;
    }
    const std::optional<std::int64_t> as_i64 = narrow<std::int64_t>(*secs);
    if (!as_i64.has_value()) {
      return std::nullopt;
    }
    return *as_i64 * kNanosPerSecond;
  }
  // RFC 3339 UTC: YYYY-MM-DDTHH:MM:SS[.fffffffff]Z
  if (text.size() < 20) {
    return std::nullopt;
  }
  if (text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') || text[13] != ':' ||
      text[16] != ':') {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> year = fixed_uint(text.substr(0, 4));
  const std::optional<std::uint32_t> month = fixed_uint(text.substr(5, 2));
  const std::optional<std::uint32_t> day = fixed_uint(text.substr(8, 2));
  const std::optional<std::uint32_t> hour = fixed_uint(text.substr(11, 2));
  const std::optional<std::uint32_t> minute = fixed_uint(text.substr(14, 2));
  const std::optional<std::uint32_t> second = fixed_uint(text.substr(17, 2));
  if (!year || !month || !day || !hour || !minute || !second) {
    return std::nullopt;
  }
  if (*month < 1u || *month > 12u || *day < 1u || *day > 31u || *hour > 23u || *minute > 59u ||
      *second > 60u) {
    return std::nullopt;
  }
  std::size_t pos = 19;
  std::int64_t fraction = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    std::size_t digits = 0;
    while (pos + digits < text.size() && text[pos + digits] >= '0' && text[pos + digits] <= '9' &&
           digits < 9) {
      fraction = fraction * 10 + static_cast<std::int64_t>(text[pos + digits] - '0');
      ++digits;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    for (std::size_t pad = digits; pad < 9; ++pad) {
      fraction *= 10;
    }
    pos += digits;
  }
  if (pos >= text.size()) {
    return std::nullopt;
  }
  if (text[pos] == 'Z' || text[pos] == 'z') {
    ++pos;
  } else if ((text[pos] == '+' || text[pos] == '-') && text.size() >= pos + 6 &&
             text[pos + 3] == ':') {
    // Offset present: require zero offset, the runtime is UTC-only by contract.
    const std::optional<std::uint32_t> oh = fixed_uint(text.substr(pos + 1, 2));
    const std::optional<std::uint32_t> om = fixed_uint(text.substr(pos + 4, 2));
    if (!oh || !om || *oh != 0u || *om != 0u) {
      return std::nullopt;
    }
    pos += 6;
  } else {
    return std::nullopt;
  }
  if (pos != text.size()) {
    return std::nullopt;
  }
  const std::int64_t days = days_from_civil(static_cast<std::int64_t>(*year), *month, *day);
  const std::int64_t seconds =
      days * kSecondsPerDay + static_cast<std::int64_t>(*hour) * 3600 +
      static_cast<std::int64_t>(*minute) * 60 + static_cast<std::int64_t>(*second);
  return seconds * kNanosPerSecond + fraction;
}

std::string format_duration(Nanos value) {
  const bool negative = value < 0;
  std::uint64_t magnitude = static_cast<std::uint64_t>(negative ? -value : value);
  std::string out;
  if (negative) {
    out.push_back('-');
  }
  const auto seconds = static_cast<std::uint64_t>(magnitude / static_cast<std::uint64_t>(kNanosPerSecond));
  const auto remainder = static_cast<std::uint64_t>(magnitude % static_cast<std::uint64_t>(kNanosPerSecond));
  if (magnitude < 1000u) {
    out += std::to_string(magnitude);
    out += "ns";
    return out;
  }
  if (magnitude < 1000000u) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu.%03lluus",
                  static_cast<unsigned long long>(magnitude / 1000u),
                  static_cast<unsigned long long>(magnitude % 1000u));
    std::string formatted(buffer);
    while (!formatted.empty() && formatted.back() == '0') {
      formatted.pop_back();
    }
    out += formatted;
    return out;
  }
  if (magnitude < 1000000000u) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu.%06llums",
                  static_cast<unsigned long long>(magnitude / 1000000u),
                  static_cast<unsigned long long>(magnitude % 1000000u));
    std::string formatted(buffer);
    while (!formatted.empty() && formatted.back() == '0') {
      formatted.pop_back();
    }
    out += formatted;
    return out;
  }
  if (seconds >= 3600u) {
    out += std::to_string(seconds / 3600u);
    out += "h";
  }
  if (seconds >= 60u) {
    out += std::to_string((seconds % 3600u) / 60u);
    out += "m";
  }
  if (seconds > 0u || remainder > 0u) {
    if (seconds != 0u || remainder != 0u) {
      const std::uint64_t secs_part = seconds % 60u;
      if (remainder == 0u) {
        out += std::to_string(secs_part);
        out += "s";
      } else {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%llu.%09llus",
                      static_cast<unsigned long long>(secs_part),
                      static_cast<unsigned long long>(remainder));
        std::string formatted(buffer);
        while (!formatted.empty() && formatted.back() == '0') {
          formatted.pop_back();
        }
        out += formatted;
      }
    }
  } else {
    out += "0s";
  }
  return out;
}

}  // namespace mf
