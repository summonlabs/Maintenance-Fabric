// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/topology.hpp"

#include <algorithm>
#include <set>

#include "mf/core/hash.hpp"
#include "mf/core/limits.hpp"

namespace mf {
namespace {

void push_unique(std::vector<TargetId>& out, TargetId id) {
  if (std::find(out.begin(), out.end(), id) == out.end()) {
    out.push_back(id);
  }
}

}  // namespace

void Topology::touch() noexcept { revision_ = revision_.next(); }

Status Topology::add_domain(DomainRecord record) {
  if (!record.id.valid()) {
    return invalid_argument("domain id is not valid");
  }
  if (record.name.size() > limits::kMaxNameLength) {
    return make_error(ErrorCode::LimitExceeded, "domain name exceeds maximum length");
  }
  if (domains_.size() >= limits::kMaxDomainsPerTopology && domains_.find(record.id) == domains_.end()) {
    return make_error(ErrorCode::LimitExceeded, "topology reached the maximum number of domains");
  }
  domains_[record.id] = std::move(record);
  touch();
  return ok_status();
}

Status Topology::add_target(TargetRecord record) {
  const Status valid = validate_target_record(record);
  if (!valid.ok()) {
    return valid;
  }
  if (targets_.size() >= limits::kMaxTargetsPerTopology &&
      targets_.find(record.id) == targets_.end()) {
    return make_error(ErrorCode::LimitExceeded, "topology reached the maximum number of targets");
  }
  if (record.parent.has_value()) {
    if (*record.parent == record.id) {
      return invalid_argument("target cannot be its own parent");
    }
    if (targets_.find(*record.parent) == targets_.end()) {
      return make_error(ErrorCode::NotFound,
                        "parent target " + to_string(*record.parent) +
                            " must exist before its child");
    }
  }
  for (const DomainId domain : record.domains) {
    if (!domain.valid()) {
      return invalid_argument("target references an invalid failure domain");
    }
    if (domains_.find(domain) == domains_.end()) {
      return make_error(ErrorCode::NotFound,
                        "failure domain " + to_string(domain) +
                            " must be declared before the targets that belong to it");
    }
  }

  const TargetId id = record.id;
  auto existing = targets_.find(id);
  if (existing != targets_.end()) {
    // Replace: drop the previous indexes for this target.
    for (const DomainId domain : existing->second.domains) {
      std::vector<TargetId>& members = domain_members_[domain];
      members.erase(std::remove(members.begin(), members.end(), id), members.end());
    }
    if (existing->second.parent.has_value()) {
      std::vector<TargetId>& siblings = children_[*existing->second.parent];
      siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
    }
  }
  targets_[id] = std::move(record);
  for (const DomainId domain : targets_[id].domains) {
    std::vector<TargetId>& members = domain_members_[domain];
    if (std::find(members.begin(), members.end(), id) == members.end()) {
      members.push_back(id);
      std::sort(members.begin(), members.end());
    }
  }
  if (targets_[id].parent.has_value()) {
    std::vector<TargetId>& siblings = children_[*targets_[id].parent];
    if (std::find(siblings.begin(), siblings.end(), id) == siblings.end()) {
      siblings.push_back(id);
      std::sort(siblings.begin(), siblings.end());
    }
  }
  touch();
  return ok_status();
}

Status Topology::add_pool(PoolRecord record) {
  if (!record.id.valid()) {
    return invalid_argument("pool id is not valid");
  }
  if (record.name.size() > limits::kMaxNameLength) {
    return make_error(ErrorCode::LimitExceeded, "pool name exceeds maximum length");
  }
  if (record.members.size() > limits::kMaxMembersPerPool) {
    return make_error(ErrorCode::LimitExceeded, "pool exceeds the maximum member count");
  }
  if (pools_.size() >= limits::kMaxPoolsPerTopology && pools_.find(record.id) == pools_.end()) {
    return make_error(ErrorCode::LimitExceeded, "topology reached the maximum number of pools");
  }
  if (record.members.empty()) {
    return invalid_argument("pool must have at least one member");
  }
  for (const TargetId member : record.members) {
    if (targets_.find(member) == targets_.end()) {
      return make_error(ErrorCode::NotFound,
                        "pool member " + to_string(member) + " is not a declared target");
    }
  }
  std::vector<TargetId> sorted(record.members);
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return invalid_argument("pool lists the same member twice");
  }

  auto existing = pools_.find(record.id);
  if (existing != pools_.end()) {
    for (const TargetId member : existing->second.members) {
      std::vector<PoolId>& pools = target_pools_[member];
      pools.erase(std::remove(pools.begin(), pools.end(), record.id), pools.end());
    }
  }
  const PoolId id = record.id;
  pools_[id] = std::move(record);
  for (const TargetId member : pools_[id].members) {
    std::vector<PoolId>& pools = target_pools_[member];
    if (std::find(pools.begin(), pools.end(), id) == pools.end()) {
      pools.push_back(id);
      std::sort(pools.begin(), pools.end());
    }
  }
  touch();
  return ok_status();
}

const TargetRecord* Topology::target(TargetId id) const noexcept {
  const auto it = targets_.find(id);
  return it == targets_.end() ? nullptr : &it->second;
}

const DomainRecord* Topology::domain(DomainId id) const noexcept {
  const auto it = domains_.find(id);
  return it == domains_.end() ? nullptr : &it->second;
}

const PoolRecord* Topology::pool(PoolId id) const noexcept {
  const auto it = pools_.find(id);
  return it == pools_.end() ? nullptr : &it->second;
}

std::vector<TargetId> Topology::domain_members(DomainId id) const {
  const auto it = domain_members_.find(id);
  return it == domain_members_.end() ? std::vector<TargetId>{} : it->second;
}

std::vector<PoolId> Topology::pools_containing(TargetId id) const {
  const auto it = target_pools_.find(id);
  return it == target_pools_.end() ? std::vector<PoolId>{} : it->second;
}

std::vector<TargetId> Topology::children(TargetId id) const {
  const auto it = children_.find(id);
  return it == children_.end() ? std::vector<TargetId>{} : it->second;
}

std::vector<TargetId> Topology::ancestors(TargetId id) const {
  std::vector<TargetId> out;
  const TargetRecord* record = target(id);
  std::size_t depth = 0;
  while (record != nullptr && record->parent.has_value() && depth < limits::kMaxTopologyChainDepth) {
    out.push_back(*record->parent);
    record = target(*record->parent);
    ++depth;
  }
  return out;
}

std::vector<TargetId> Topology::subtree(TargetId id) const {
  std::vector<TargetId> out;
  if (target(id) == nullptr) {
    return out;
  }
  std::vector<TargetId> stack{id};
  std::size_t guard = 0;
  while (!stack.empty()) {
    const TargetId current = stack.back();
    stack.pop_back();
    out.push_back(current);
    if (++guard > limits::kMaxTargetsPerTopology) {
      break;
    }
    const std::vector<TargetId> kids = children(current);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<TargetId> Topology::removal_set(const std::vector<TargetId>& requested) const {
  std::vector<TargetId> out;
  for (const TargetId id : requested) {
    for (const TargetId member : subtree(id)) {
      push_unique(out, member);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<DomainId> Topology::domains_of(const std::vector<TargetId>& targets) const {
  std::vector<DomainId> out;
  for (const TargetId id : targets) {
    const TargetRecord* record = target(id);
    if (record == nullptr) {
      continue;
    }
    for (const DomainId domain : record->domains) {
      if (std::find(out.begin(), out.end(), domain) == out.end()) {
        out.push_back(domain);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PoolId> Topology::pools_of(const std::vector<TargetId>& targets) const {
  std::vector<PoolId> out;
  for (const TargetId id : targets) {
    for (const PoolId pool : pools_containing(id)) {
      if (std::find(out.begin(), out.end(), pool) == out.end()) {
        out.push_back(pool);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

Status Topology::validate() const {
  if (targets_.size() > limits::kMaxTargetsPerTopology) {
    return make_error(ErrorCode::LimitExceeded, "too many targets");
  }
  if (domains_.size() > limits::kMaxDomainsPerTopology) {
    return make_error(ErrorCode::LimitExceeded, "too many failure domains");
  }
  if (pools_.size() > limits::kMaxPoolsPerTopology) {
    return make_error(ErrorCode::LimitExceeded, "too many pools");
  }
  for (const auto& [id, record] : targets_) {
    const Status valid = validate_target_record(record);
    if (!valid.ok()) {
      return valid;
    }
    if (record.parent.has_value()) {
      if (*record.parent == id || targets_.find(*record.parent) == targets_.end()) {
        return make_error(ErrorCode::CorruptState,
                          "target " + to_string(id) + " has an unresolved parent");
      }
    }
    for (const DomainId domain : record.domains) {
      if (domains_.find(domain) == domains_.end()) {
        return make_error(ErrorCode::CorruptState,
                          "target " + to_string(id) + " references undeclared domain " +
                              to_string(domain));
      }
    }
    // Ancestor chain must terminate.
    std::set<TargetId> seen;
    const TargetRecord* cursor = &record;
    while (cursor->parent.has_value()) {
      if (!seen.insert(cursor->id).second) {
        return make_error(ErrorCode::CorruptState,
                          "target " + to_string(id) + " is part of a containment cycle");
      }
      cursor = target(*cursor->parent);
      if (cursor == nullptr) {
        return make_error(ErrorCode::CorruptState,
                          "target " + to_string(id) + " has a dangling parent chain");
      }
    }
  }
  for (const auto& [id, record] : pools_) {
    if (record.members.empty()) {
      return make_error(ErrorCode::CorruptState, "pool " + to_string(id) + " has no members");
    }
    for (const TargetId member : record.members) {
      if (targets_.find(member) == targets_.end()) {
        return make_error(ErrorCode::CorruptState,
                          "pool " + to_string(id) + " references undeclared target " +
                              to_string(member));
      }
    }
  }
  return ok_status();
}

std::uint64_t Topology::digest() const {
  Digest digest;
  digest.update("mf.topology.v1");
  digest.update_u64(targets_.size());
  for (const auto& [id, record] : targets_) {
    digest.update("T");
    digest.update_u32(static_cast<std::uint32_t>(id.kind));
    digest.update_u32(id.index);
    digest.update(record.name);
    digest.update_bool(record.parent.has_value());
    if (record.parent.has_value()) {
      digest.update_u32(static_cast<std::uint32_t>(record.parent->kind));
      digest.update_u32(record.parent->index);
    }
    digest.update_u64(record.domains.size());
    for (const DomainId domain : record.domains) {
      digest.update_u32(static_cast<std::uint32_t>(domain.kind));
      digest.update_u32(domain.index);
    }
    digest.update_u32(record.capacity_units);
    digest.update_bool(record.maintainable);
  }
  digest.update_u64(domains_.size());
  for (const auto& [id, record] : domains_) {
    digest.update("D");
    digest.update_u32(static_cast<std::uint32_t>(id.kind));
    digest.update_u32(id.index);
    digest.update(record.name);
    digest.update_bool(record.correlated);
  }
  digest.update_u64(pools_.size());
  for (const auto& [id, record] : pools_) {
    digest.update("P");
    digest.update_u64(id.value());
    digest.update(record.name);
    digest.update_u64(record.members.size());
    for (const TargetId member : record.members) {
      digest.update_u32(static_cast<std::uint32_t>(member.kind));
      digest.update_u32(member.index);
    }
  }
  return digest.value();
}

}  // namespace mf
