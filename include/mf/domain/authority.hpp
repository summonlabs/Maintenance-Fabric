#pragma once

// Maintenance authority is the right to hold capacity out of service. Two jobs
// that are each individually admissible may still be jointly unsafe when their
// removal sets share a correlated failure domain; the ledger is the single
// serialization point that prevents them from independently consuming the same
// tolerance budget.
//
// Authority is incarnation owned: a reservation granted by a previous process
// incarnation is revoked on restart and can never be released or renewed by the
// new incarnation.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/identity.hpp"

namespace mf {

struct AuthorityReservation {
  AuthorityId id;
  JobId job;
  GenerationId generation;
  AttemptId attempt;
  ControllerIncarnation owner;
  std::vector<TargetId> targets;
  std::vector<DomainId> domains;
  std::vector<PoolId> pools;
  Revision revision;
  Nanos granted_at{0};
  Nanos expires_at{0};
  std::uint64_t digest{0};

  [[nodiscard]] bool live_at(Nanos now) const noexcept { return now < expires_at; }
  [[nodiscard]] bool covers(TargetId target) const noexcept;
  [[nodiscard]] std::string to_text() const;
};

class AuthorityLedger {
 public:
  explicit AuthorityLedger(Nanos default_lease = limits::kDefaultAuthorityLeaseNanos);

  /// Grants a reservation. The caller must have already verified, under the
  /// same lock, that the union of this job's removal set with every live
  /// reservation still satisfies policy. Refuses a second reservation for the
  /// same (job, generation) and refuses when the live set is at capacity.
  [[nodiscard]] Result<AuthorityId> reserve(AuthorityReservation reservation);

  /// Recovery path: installs a reservation exactly as persisted and advances
  /// the id allocator past it.
  [[nodiscard]] Status adopt(AuthorityReservation reservation);

  /// Unconditional removal used by recovery, which has no live incarnation to
  /// match against the persisted owner.
  [[nodiscard]] std::size_t erase(AuthorityId id);

  [[nodiscard]] Status release(AuthorityId id, const ControllerIncarnation& current);
  [[nodiscard]] Status renew(AuthorityId id, const AttemptId& attempt,
                             const ControllerIncarnation& current, Nanos extend, Nanos now);
  [[nodiscard]] Status consume(AuthorityId id, const ControllerIncarnation& current);

  /// Removes reservations whose lease has expired. Returns the number removed.
  [[nodiscard]] std::size_t expire(Nanos now);

  /// Revokes every reservation owned by an incarnation fenced by p current.
  /// Called during recovery so a restarted process never inherits authority it
  /// did not grant. Returns the revoked reservations, ascending by id.
  [[nodiscard]] std::vector<AuthorityReservation> fence_older_incarnations(
      const ControllerIncarnation& current);

  [[nodiscard]] const AuthorityReservation* find(AuthorityId id) const noexcept;
  [[nodiscard]] const AuthorityReservation* find_for_job(JobId job, GenerationId generation) const
      noexcept;
  [[nodiscard]] std::vector<const AuthorityReservation*> live(Nanos now) const;
  /// Union of the removal sets of every live reservation, ascending.
  [[nodiscard]] std::vector<TargetId> reserved_targets(Nanos now) const;

  [[nodiscard]] std::size_t size() const noexcept { return reservations_.size(); }
  [[nodiscard]] Nanos default_lease() const noexcept { return default_lease_; }
  [[nodiscard]] std::uint64_t granted() const noexcept { return granted_; }
  [[nodiscard]] std::uint64_t denied() const noexcept { return denied_; }
  [[nodiscard]] std::uint64_t expired_count() const noexcept { return expired_; }
  [[nodiscard]] const std::map<AuthorityId, AuthorityReservation>& reservations() const noexcept {
    return reservations_;
  }

  void clear();

 private:
  std::map<AuthorityId, AuthorityReservation> reservations_;
  AuthorityId next_id_{AuthorityId::from_u64(1)};
  Nanos default_lease_;
  std::uint64_t granted_{0};
  std::uint64_t denied_{0};
  std::uint64_t expired_{0};
};

[[nodiscard]] std::uint64_t reservation_digest(const AuthorityReservation& reservation);

}  // namespace mf
