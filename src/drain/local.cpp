// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/drain/local.hpp"

#include <algorithm>

namespace mf {

LocalDrainPort::LocalDrainPort(LocalDrainOptions options) : options_(options) {}

LocalDrainPort::~LocalDrainPort() = default;

void LocalDrainPort::fail_next(std::uint32_t count) {
  std::lock_guard<std::mutex> guard(mu_);
  options_.fail_next_requests = count;
}

void LocalDrainPort::set_granted_state(DrainState state) {
  std::lock_guard<std::mutex> guard(mu_);
  options_.granted_state = state;
}

void LocalDrainPort::drop_leases_on_next_query() {
  std::lock_guard<std::mutex> guard(mu_);
  options_.drop_leases_on_next_query = true;
}

std::size_t LocalDrainPort::active_leases() const {
  std::lock_guard<std::mutex> guard(mu_);
  return leases_.size();
}

std::uint64_t LocalDrainPort::requests() const {
  std::lock_guard<std::mutex> guard(mu_);
  return requests_;
}

std::uint64_t LocalDrainPort::releases() const {
  std::lock_guard<std::mutex> guard(mu_);
  return releases_;
}

std::uint64_t LocalDrainPort::refreshes() const {
  std::lock_guard<std::mutex> guard(mu_);
  return refreshes_;
}

bool LocalDrainPort::holds(DrainLeaseId lease) const {
  std::lock_guard<std::mutex> guard(mu_);
  return leases_.find(lease) != leases_.end();
}

DrainResponse LocalDrainPort::request_drain(const DrainRequest& request) {
  DrainResponse response;
  const Status valid = validate_drain_request(request);
  if (!valid.ok()) {
    response.status = valid;
    return response;
  }
  std::lock_guard<std::mutex> guard(mu_);
  ++requests_;
  if (options_.fail_next_requests > 0) {
    --options_.fail_next_requests;
    response.status = make_error(ErrorCode::DrainFailed,
                                 "drain service rejected the request (injected failure)");
    response.lease.targets = request.targets;
    response.lease.state = DrainState::Failed;
    response.lease.attempt = request.attempt;
    response.lease.owner = request.requester;
    return response;
  }
  // A second request for the same job/generation/attempt is idempotent and
  // returns the lease that already exists.
  for (auto& [id, lease] : leases_) {
    if (lease.attempt == request.attempt) {
      response.lease = lease;
      response.status = ok_status();
      return response;
    }
  }
  DrainLease lease;
  lease.id = next_id_;
  next_id_ = next_id_.next();
  lease.targets = request.targets;
  std::sort(lease.targets.begin(), lease.targets.end());
  lease.granted_at = 0;
  lease.expires_at = request.lease_duration;
  lease.state = options_.granted_state;
  lease.attempt = request.attempt;
  lease.owner = request.requester;
  lease.epoch = ++epoch_;
  leases_.emplace(lease.id, lease);
  response.lease = lease;
  response.status = ok_status();
  return response;
}

DrainResponse LocalDrainPort::refresh_drain(DrainLeaseId lease, Nanos extend,
                                            const ControllerIncarnation& requester) {
  DrainResponse response;
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = leases_.find(lease);
  if (it == leases_.end()) {
    response.status = make_error(ErrorCode::DrainLeaseLost,
                                 "drain lease " + to_string(lease) + " is not held");
    response.lease.id = lease;
    response.lease.state = DrainState::NotFound;
    return response;
  }
  if (options_.fence_refresh_by_owner && it->second.owner != requester) {
    response.status = make_error(ErrorCode::StaleIncarnation,
                                 "drain lease " + to_string(lease) + " is owned by " +
                                     to_string(it->second.owner));
    response.lease = it->second;
    return response;
  }
  if (extend <= 0) {
    response.status = invalid_argument("drain refresh must extend by a positive duration");
    response.lease = it->second;
    return response;
  }
  ++refreshes_;
  it->second.expires_at += extend;
  it->second.owner = requester;
  it->second.epoch = ++epoch_;
  response.lease = it->second;
  response.status = ok_status();
  return response;
}

DrainResponse LocalDrainPort::query_drain(DrainLeaseId lease,
                                          const ControllerIncarnation& requester) {
  DrainResponse response;
  std::lock_guard<std::mutex> guard(mu_);
  if (options_.drop_leases_on_next_query) {
    options_.drop_leases_on_next_query = false;
    leases_.clear();
  }
  const auto it = leases_.find(lease);
  if (it == leases_.end()) {
    response.status = make_error(ErrorCode::DrainLeaseLost,
                                 "drain lease " + to_string(lease) + " is not held");
    response.lease.id = lease;
    response.lease.state = DrainState::NotFound;
    return response;
  }
  (void)requester;
  response.lease = it->second;
  response.status = ok_status();
  return response;
}

Status LocalDrainPort::release_drain(DrainLeaseId lease, const ControllerIncarnation& requester) {
  std::lock_guard<std::mutex> guard(mu_);
  (void)requester;
  const auto it = leases_.find(lease);
  if (it == leases_.end()) {
    return make_error(ErrorCode::NotFound, "drain lease " + to_string(lease) + " is not held");
  }
  leases_.erase(it);
  ++releases_;
  return ok_status();
}

}  // namespace mf
