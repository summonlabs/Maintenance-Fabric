#pragma once

// Strict, line-oriented configuration text for topology and policy. The format
// is deliberately small: one record per line, whitespace separated key=value
// tokens, '#' comments. Unknown record kinds, unknown keys, duplicate keys and
// malformed values are rejected with the line number, so a typo can never be
// silently ignored.

#include <cstddef>
#include <string>
#include <string_view>

#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"

namespace mf {

struct ParseStats {
  std::size_t lines{0};
  std::size_t records{0};
};

[[nodiscard]] Result<Topology> load_topology_text(std::string_view text, ParseStats& stats);
[[nodiscard]] Result<Topology> load_topology_file(const std::string& path, ParseStats& stats);
[[nodiscard]] Result<Policy> load_policy_text(std::string_view text, Nanos reference_now,
                                              ParseStats& stats);
[[nodiscard]] Result<Policy> load_policy_file(const std::string& path, Nanos reference_now,
                                              ParseStats& stats);

/// Parses "30s", "5m", "2h", "250ms", "900ns" or a bare nanosecond count.
[[nodiscard]] std::optional<Nanos> parse_duration_text(std::string_view text);

/// Renders a policy back into the same text format (round-trip inspection).
[[nodiscard]] std::string render_policy(const Policy& policy);
[[nodiscard]] std::string render_topology(const Topology& topology);

}  // namespace mf
