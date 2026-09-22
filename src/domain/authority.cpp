// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/authority.hpp"

#include <algorithm>

#include "mf/core/hash.hpp"

namespace mf {

bool AuthorityReservation::covers(TargetId target) const noexcept {
  return std::find(targets.begin(), targets.end(), target) != targets.end();
}

std::string AuthorityReservation::to_text() const {
  std::string out = to_string(id);
  out += " job=";
  out += to_string(job);
  out += " gen=";
  out += to_string(generation);
  out += " owner=";
  out += to_string(owner);
  out += " targets=";
  out += std::to_string(targets.size());
  out += " expires=";
  out += format_time(expires_at);
  return out;
}

std::uint64_t reservation_digest(const AuthorityReservation& reservation) {
  Digest digest;
  digest.update("mf.authority.v1");
  digest.update_u64(reservation.id.value());
  digest.update_u64(reservation.job.value());
  digest.update_u64(reservation.generation.value());
  digest.update(to_string(reservation.attempt));
  digest.update(to_string(reservation.owner));
  digest.update_u64(reservation.targets.size());
  for (const TargetId target : reservation.targets) {
    digest.update_u32(static_cast<std::uint32_t>(target.kind));
    digest.update_u32(target.index);
  }
  digest.update_u64(reservation.domains.size());
  for (const DomainId domain : reservation.domains) {
    digest.update_u32(static_cast<std::uint32_t>(domain.kind));
    digest.update_u32(domain.index);
  }
  digest.update_u64(reservation.pools.size());
  for (const PoolId pool : reservation.pools) {
    digest.update_u64(pool.value());
  }
  digest.update_i64(reservation.granted_at);
  digest.update_i64(reservation.expires_at);
  return digest.value();
}

AuthorityLedger::AuthorityLedger(Nanos default_lease)
    : default_lease_(default_lease <= 0 ? limits::kDefaultAuthorityLeaseNanos : default_lease) {}

Result<AuthorityId> AuthorityLedger::reserve(AuthorityReservation reservation) {
  if (!reservation.job.valid() || !reservation.generation.valid()) {
    ++denied_;
    return invalid_argument("authority reservation is missing a job or generation");
  }
  if (!reservation.attempt.valid()) {
    ++denied_;
    return invalid_argument("authority reservation is missing an attempt identity");
  }
  if (!reservation.owner.valid()) {
    ++denied_;
    return invalid_argument("authority reservation is missing an owning incarnation");
  }
  if (reservation.targets.empty()) {
    ++denied_;
    return invalid_argument("authority reservation covers no targets");
  }
  if (reservation.expires_at <= reservation.granted_at) {
    ++denied_;
    return invalid_argument("authority reservation lease is not positive");
  }
  if (reservation.expires_at - reservation.granted_at > limits::kMaxAuthorityLeaseNanos) {
    ++denied_;
    return invalid_argument("authority reservation lease exceeds the maximum");
  }
  if (find_for_job(reservation.job, reservation.generation) != nullptr) {
    ++denied_;
    return make_error(ErrorCode::AlreadyExists,
                      "job " + to_string(reservation.job) + " generation " +
                          to_string(reservation.generation) + " already holds maintenance authority");
  }
  if (reservations_.size() >= limits::kMaxLiveReservations) {
    ++denied_;
    return make_error(ErrorCode::LimitExceeded, "authority ledger is at capacity");
  }

  reservation.id = next_id_;
  next_id_ = next_id_.next();
  reservation.revision = Revision::from_u64(1);
  reservation.digest = reservation_digest(reservation);
  const AuthorityId id = reservation.id;
  reservations_.emplace(id, std::move(reservation));
  ++granted_;
  return id;
}

Status AuthorityLedger::adopt(AuthorityReservation reservation) {
  if (!reservation.id.valid() || !reservation.job.valid()) {
    return invalid_argument("persisted authority reservation is not well formed");
  }
  if (reservation.digest == 0) {
    reservation.digest = reservation_digest(reservation);
  }
  if (reservation.id >= next_id_) {
    next_id_ = reservation.id.next();
  }
  reservations_[reservation.id] = std::move(reservation);
  return ok_status();
}

std::size_t AuthorityLedger::erase(AuthorityId id) { return reservations_.erase(id); }

Status AuthorityLedger::release(AuthorityId id, const ControllerIncarnation& current) {
  const auto it = reservations_.find(id);
  if (it == reservations_.end()) {
    return make_error(ErrorCode::NotFound, "authority reservation " + to_string(id) + " is not held");
  }
  if (it->second.owner != current) {
    return make_error(ErrorCode::StaleIncarnation,
                      "authority reservation " + to_string(id) + " is owned by incarnation " +
                          to_string(it->second.owner) + ", not " + to_string(current));
  }
  reservations_.erase(it);
  return ok_status();
}

Status AuthorityLedger::renew(AuthorityId id, const AttemptId& attempt,
                              const ControllerIncarnation& current, Nanos extend, Nanos now) {
  const auto it = reservations_.find(id);
  if (it == reservations_.end()) {
    return make_error(ErrorCode::NotFound, "authority reservation " + to_string(id) + " is not held");
  }
  if (it->second.owner != current) {
    return make_error(ErrorCode::StaleIncarnation,
                      "authority reservation " + to_string(id) + " is owned by incarnation " +
                          to_string(it->second.owner));
  }
  if (it->second.attempt != attempt) {
    return make_error(ErrorCode::StaleAttempt,
                      "authority reservation " + to_string(id) + " belongs to attempt " +
                          to_string(it->second.attempt) + ", not " + to_string(attempt));
  }
  if (extend <= 0) {
    return invalid_argument("authority renewal must extend by a positive duration");
  }
  const Nanos base = it->second.expires_at > now ? it->second.expires_at : now;
  if (extend > limits::kMaxAuthorityLeaseNanos - (base - now)) {
    return invalid_argument("authority renewal would exceed the maximum lease");
  }
  it->second.expires_at = base + extend;
  it->second.revision = it->second.revision.next();
  it->second.digest = reservation_digest(it->second);
  return ok_status();
}

Status AuthorityLedger::consume(AuthorityId id, const ControllerIncarnation& current) {
  const auto it = reservations_.find(id);
  if (it == reservations_.end()) {
    return make_error(ErrorCode::NotFound, "authority reservation " + to_string(id) + " is not held");
  }
  if (it->second.owner != current) {
    return make_error(ErrorCode::StaleIncarnation,
                      "authority reservation " + to_string(id) + " is owned by incarnation " +
                          to_string(it->second.owner));
  }
  return ok_status();
}

std::size_t AuthorityLedger::expire(Nanos now) {
  std::size_t removed = 0;
  for (auto it = reservations_.begin(); it != reservations_.end();) {
    if (!it->second.live_at(now)) {
      it = reservations_.erase(it);
      ++removed;
      ++expired_;
    } else {
      ++it;
    }
  }
  return removed;
}

std::vector<AuthorityReservation> AuthorityLedger::fence_older_incarnations(
    const ControllerIncarnation& current) {
  std::vector<AuthorityReservation> revoked;
  for (auto it = reservations_.begin(); it != reservations_.end();) {
    if (is_fenced_by(it->second.owner, current)) {
      revoked.push_back(it->second);
      it = reservations_.erase(it);
    } else {
      ++it;
    }
  }
  std::sort(revoked.begin(), revoked.end(),
            [](const AuthorityReservation& a, const AuthorityReservation& b) { return a.id < b.id; });
  return revoked;
}

const AuthorityReservation* AuthorityLedger::find(AuthorityId id) const noexcept {
  const auto it = reservations_.find(id);
  return it == reservations_.end() ? nullptr : &it->second;
}

const AuthorityReservation* AuthorityLedger::find_for_job(JobId job,
                                                          GenerationId generation) const noexcept {
  for (const auto& [id, reservation] : reservations_) {
    (void)id;
    if (reservation.job == job && reservation.generation == generation) {
      return &reservation;
    }
  }
  return nullptr;
}

std::vector<const AuthorityReservation*> AuthorityLedger::live(Nanos now) const {
  std::vector<const AuthorityReservation*> out;
  for (const auto& [id, reservation] : reservations_) {
    (void)id;
    if (reservation.live_at(now)) {
      out.push_back(&reservation);
    }
  }
  return out;
}

std::vector<TargetId> AuthorityLedger::reserved_targets(Nanos now) const {
  std::vector<TargetId> out;
  for (const auto& [id, reservation] : reservations_) {
    (void)id;
    if (!reservation.live_at(now)) {
      continue;
    }
    for (const TargetId target : reservation.targets) {
      if (std::find(out.begin(), out.end(), target) == out.end()) {
        out.push_back(target);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

void AuthorityLedger::clear() {
  reservations_.clear();
  next_id_ = AuthorityId::from_u64(1);
  granted_ = 0;
  denied_ = 0;
  expired_ = 0;
}

}  // namespace mf
