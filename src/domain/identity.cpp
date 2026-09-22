// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/identity.hpp"

#include <array>
#include <utility>

namespace mf {
namespace {

struct KindName {
  TargetKind kind;
  std::string_view name;
};

constexpr std::array<KindName, kTargetKindCount> kTargetKindNames{{
    {TargetKind::Unknown, "unknown"},
    {TargetKind::Link, "link"},
    {TargetKind::Port, "port"},
    {TargetKind::Switch, "switch"},
    {TargetKind::Rack, "rack"},
    {TargetKind::Pod, "pod"},
    {TargetKind::Site, "site"},
    {TargetKind::ControlPlane, "control-plane"},
}};

struct DomainKindName {
  DomainKind kind;
  std::string_view name;
};

constexpr std::array<DomainKindName, kDomainKindCount> kDomainKindNames{{
    {DomainKind::Unknown, "unknown"},
    {DomainKind::Site, "site"},
    {DomainKind::Pod, "pod"},
    {DomainKind::Rack, "rack"},
    {DomainKind::Power, "power"},
    {DomainKind::Plane, "plane"},
    {DomainKind::Zone, "zone"},
    {DomainKind::Custom, "custom"},
}};

}  // namespace

const char* to_string(TargetKind kind) noexcept {
  for (const KindName& entry : kTargetKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept {
  for (const KindName& entry : kTargetKindNames) {
    if (entry.name == text) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

const char* to_string(DomainKind kind) noexcept {
  for (const DomainKindName& entry : kDomainKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

std::optional<DomainKind> parse_domain_kind(std::string_view text) noexcept {
  for (const DomainKindName& entry : kDomainKindNames) {
    if (entry.name == text) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::string_view, std::uint32_t>> split_prefixed_index(
    std::string_view text) noexcept {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> index = parse_u32(text.substr(colon + 1));
  if (!index.has_value()) {
    return std::nullopt;
  }
  return std::make_pair(text.substr(0, colon), *index);
}

std::string to_string(const TargetId& id) {
  std::string out(to_string(id.kind));
  out.push_back(':');
  out += std::to_string(id.index);
  return out;
}

std::optional<TargetId> parse_target_id(std::string_view text) {
  const std::optional<std::pair<std::string_view, std::uint32_t>> parts =
      split_prefixed_index(text);
  if (!parts.has_value()) {
    return std::nullopt;
  }
  const std::optional<TargetKind> kind = parse_target_kind(parts->first);
  if (!kind.has_value() || *kind == TargetKind::Unknown) {
    return std::nullopt;
  }
  return TargetId{*kind, parts->second};
}

std::string to_string(const DomainId& id) {
  std::string out(to_string(id.kind));
  out.push_back(':');
  out += std::to_string(id.index);
  return out;
}

std::optional<DomainId> parse_domain_id(std::string_view text) {
  const std::optional<std::pair<std::string_view, std::uint32_t>> parts =
      split_prefixed_index(text);
  if (!parts.has_value()) {
    return std::nullopt;
  }
  const std::optional<DomainKind> kind = parse_domain_kind(parts->first);
  if (!kind.has_value() || *kind == DomainKind::Unknown) {
    return std::nullopt;
  }
  return DomainId{*kind, parts->second};
}

std::string to_string(const ControllerIncarnation& incarnation) {
  std::string out = mf::to_string(incarnation.controller);
  out += '/';
  out += mf::to_string(incarnation.boot_epoch);
  out += '/';
  out += std::to_string(incarnation.nonce);
  return out;
}

std::optional<ControllerIncarnation> parse_controller_incarnation(std::string_view text) {
  ControllerIncarnation out;
  std::size_t pos = 0;
  int part = 0;
  while (pos <= text.size()) {
    const std::size_t slash = text.find('/', pos);
    const std::string_view piece =
        slash == std::string_view::npos ? text.substr(pos) : text.substr(pos, slash - pos);
    switch (part) {
      case 0: {
        const std::optional<ControllerId> v = parse_id<ControllerTag>(piece);
        if (!v.has_value()) return std::nullopt;
        out.controller = *v;
        break;
      }
      case 1: {
        const std::optional<BootEpoch> v = parse_id<BootEpochTag>(piece);
        if (!v.has_value()) return std::nullopt;
        out.boot_epoch = *v;
        break;
      }
      case 2: {
        const std::optional<std::uint64_t> v = parse_u64(piece);
        if (!v.has_value()) return std::nullopt;
        out.nonce = *v;
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
  if (part != 3 || !out.valid()) {
    return std::nullopt;
  }
  return out;
}

}  // namespace mf
