// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/id.hpp"

#include <string>

#include "mf/core/checked.hpp"
#include "mf/core/hash.hpp"

namespace mf {
namespace {

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
  if (text.empty() || text.size() > 20) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!is_digit(c)) {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return std::nullopt;
    }
    value = value * 10u + digit;
  }
  return value;
}

std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept {
  const std::optional<std::uint64_t> wide = parse_u64(text);
  if (!wide.has_value()) {
    return std::nullopt;
  }
  return narrow<std::uint32_t>(*wide);
}

RequestorId requestor_from_name(std::string_view name) noexcept {
  if (name.empty()) {
    return RequestorId::from_u64(1);
  }
  const std::uint64_t digest = fnv1a64(name);
  return RequestorId::from_u64(digest == 0 ? 1 : digest);
}

std::string to_string(const AttemptId& id) {
  std::string out = mf::to_string(id.job);
  out += '/';
  out += mf::to_string(id.generation);
  out += '/';
  out += mf::to_string(id.ordinal);
  return out;
}

std::optional<AttemptId> parse_attempt_id(std::string_view text) {
  AttemptId id;
  std::size_t pos = 0;
  int part = 0;
  while (pos <= text.size()) {
    const std::size_t slash = text.find('/', pos);
    const std::string_view piece =
        slash == std::string_view::npos ? text.substr(pos) : text.substr(pos, slash - pos);
    switch (part) {
      case 0: {
        const std::optional<JobId> v = parse_id<JobTag>(piece);
        if (!v.has_value()) return std::nullopt;
        id.job = *v;
        break;
      }
      case 1: {
        const std::optional<GenerationId> v = parse_id<GenerationTag>(piece);
        if (!v.has_value()) return std::nullopt;
        id.generation = *v;
        break;
      }
      case 2: {
        const std::optional<AttemptOrdinal> v = parse_id<AttemptTag>(piece);
        if (!v.has_value()) return std::nullopt;
        id.ordinal = *v;
        break;
      }
      default:
        return std::nullopt;
    }
    ++part;
    if (slash == std::string_view::npos) {
      break;
    }
    pos = slash + 1;
  }
  if (part != 3 || !id.valid()) {
    return std::nullopt;
  }
  return id;
}

}  // namespace mf
