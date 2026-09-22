#pragma once

// Domain identities: targets, failure domains, controller incarnations. These
// are value types with explicit validity, deterministic text form, and a total
// order so every iteration over them is reproducible.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mf/core/id.hpp"

namespace mf {

// --- Target kinds ------------------------------------------------------------

enum class TargetKind : std::uint8_t {
  Unknown = 0,
  Link = 1,
  Port = 2,
  Switch = 3,
  Rack = 4,
  Pod = 5,
  Site = 6,
  ControlPlane = 7,
};

inline constexpr std::size_t kTargetKindCount = 8;

[[nodiscard]] const char* to_string(TargetKind kind) noexcept;
[[nodiscard]] std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept;

/// Physical/administrative target. The kind participates in ordering and in
/// policy matching; a link id never equals a rack id.
struct TargetId {
  TargetKind kind{TargetKind::Unknown};
  std::uint32_t index{0};

  friend constexpr bool operator==(const TargetId&, const TargetId&) noexcept = default;
  friend constexpr auto operator<=>(const TargetId&, const TargetId&) noexcept = default;

  [[nodiscard]] constexpr bool valid() const noexcept { return kind != TargetKind::Unknown; }
};

[[nodiscard]] std::string to_string(const TargetId& id);
[[nodiscard]] std::optional<TargetId> parse_target_id(std::string_view text);

struct TargetIdHash {
  [[nodiscard]] std::size_t operator()(const TargetId& id) const noexcept {
    return (static_cast<std::size_t>(id.kind) << 32u) ^ static_cast<std::size_t>(id.index);
  }
};

// --- Failure domains ---------------------------------------------------------

enum class DomainKind : std::uint8_t {
  Unknown = 0,
  Site = 1,
  Pod = 2,
  Rack = 3,
  Power = 4,
  Plane = 5,
  Zone = 6,
  Custom = 7,
};

inline constexpr std::size_t kDomainKindCount = 8;

[[nodiscard]] const char* to_string(DomainKind kind) noexcept;
[[nodiscard]] std::optional<DomainKind> parse_domain_kind(std::string_view text) noexcept;

struct DomainId {
  DomainKind kind{DomainKind::Unknown};
  std::uint32_t index{0};

  friend constexpr bool operator==(const DomainId&, const DomainId&) noexcept = default;
  friend constexpr auto operator<=>(const DomainId&, const DomainId&) noexcept = default;

  [[nodiscard]] constexpr bool valid() const noexcept { return kind != DomainKind::Unknown; }
};

[[nodiscard]] std::string to_string(const DomainId& id);
[[nodiscard]] std::optional<DomainId> parse_domain_id(std::string_view text);

struct DomainIdHash {
  [[nodiscard]] std::size_t operator()(const DomainId& id) const noexcept {
    return (static_cast<std::size_t>(id.kind) << 32u) ^ static_cast<std::size_t>(id.index);
  }
};

/// Splits "prefix:index" and validates the prefix against a fixed table.
[[nodiscard]] std::optional<std::pair<std::string_view, std::uint32_t>> split_prefixed_index(
    std::string_view text) noexcept;

// --- Controller incarnation --------------------------------------------------

/// Identity of one controller process incarnation. A boot epoch is bumped on
/// every start; the nonce distinguishes two starts within the same epoch
/// resolution. Every durable write and every live lease carries the incarnation
/// that created it, so a restart fences everything the previous process owned.
struct ControllerIncarnation {
  ControllerId controller;
  BootEpoch boot_epoch;
  std::uint64_t nonce{0};

  friend constexpr bool operator==(const ControllerIncarnation&,
                                   const ControllerIncarnation&) noexcept = default;
  friend constexpr auto operator<=>(const ControllerIncarnation&,
                                    const ControllerIncarnation&) noexcept = default;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return controller.valid() && boot_epoch.valid() && nonce != 0;
  }
};

[[nodiscard]] std::string to_string(const ControllerIncarnation& incarnation);
[[nodiscard]] std::optional<ControllerIncarnation> parse_controller_incarnation(std::string_view text);

/// True when p record_incarnation was produced by a strictly older boot epoch
/// of the same controller, i.e. the record is fenced by a restart.
[[nodiscard]] constexpr bool is_fenced_by(const ControllerIncarnation& record_incarnation,
                                          const ControllerIncarnation& current) noexcept {
  if (record_incarnation.controller != current.controller) {
    return true;
  }
  if (record_incarnation.boot_epoch != current.boot_epoch) {
    return record_incarnation.boot_epoch < current.boot_epoch;
  }
  return record_incarnation.nonce != current.nonce;
}

}  // namespace mf
