#pragma once

// Target records describe topology structure and static capability. Operational
// health is never stored here: it is an observation with an age, and it arrives
// as evidence.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/domain/identity.hpp"

namespace mf {

enum class Health : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Down = 3,
  Retired = 4,
};

[[nodiscard]] const char* to_string(Health health) noexcept;
[[nodiscard]] std::optional<Health> parse_health(std::string_view text) noexcept;

/// True when the target is currently carrying traffic or serving its function.
[[nodiscard]] constexpr bool is_serving(Health health) noexcept {
  return health == Health::Healthy || health == Health::Degraded;
}

/// True when the target serves but with reduced headroom.
[[nodiscard]] constexpr bool is_degraded(Health health) noexcept {
  return health == Health::Degraded;
}

struct TargetRecord {
  TargetId id;
  std::string name;
  std::optional<TargetId> parent;
  std::vector<DomainId> domains;
  std::uint32_t capacity_units{1};
  bool maintainable{true};

  [[nodiscard]] bool in_domain(DomainId domain) const noexcept;
};

/// Validates field-level limits. Structural validation (parent existence,
/// duplicate ids) is the topology's responsibility.
[[nodiscard]] Status validate_target_record(const TargetRecord& record);

}  // namespace mf
