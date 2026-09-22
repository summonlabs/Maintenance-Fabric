#pragma once

// Evidence is an aged observation, never a fact. Every decision that authorizes
// service removal consumes evidence that is fresh under the current controller
// incarnation; evidence that survives a restart is not automatically fresh
// again.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/identity.hpp"
#include "mf/domain/target.hpp"

namespace mf {

enum class EvidenceKind : std::uint16_t {
  TargetHealth = 1,
  PoolRedundancy = 2,
  ContractAvailability = 3,
  DrainLeaseState = 4,
  RestorationCheck = 5,
  MaintenanceCompletion = 6,
  ControlPlaneQuorum = 7,
};

inline constexpr std::size_t kEvidenceKindCount = 7;

[[nodiscard]] const char* to_string(EvidenceKind kind) noexcept;
[[nodiscard]] std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept;

/// Volatile observations describe the live state of a resource and must be
/// re-observed in the current incarnation before they can authorize anything.
/// Durable declarations describe configured structure and survive a restart,
/// but they are still bounded by their time-to-live.
enum class EvidenceClass : std::uint8_t { Volatile = 0, Durable = 1 };

[[nodiscard]] constexpr EvidenceClass default_evidence_class(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::TargetHealth:
    case EvidenceKind::PoolRedundancy:
    case EvidenceKind::ContractAvailability:
    case EvidenceKind::DrainLeaseState:
    case EvidenceKind::RestorationCheck:
    case EvidenceKind::MaintenanceCompletion:
    case EvidenceKind::ControlPlaneQuorum:
      return EvidenceClass::Volatile;
  }
  return EvidenceClass::Volatile;
}

enum class SubjectKind : std::uint8_t {
  None = 0,
  Target = 1,
  Domain = 2,
  Pool = 3,
  Contract = 4,
  Global = 5,
};

[[nodiscard]] const char* to_string(SubjectKind kind) noexcept;

struct EvidenceSubject {
  SubjectKind kind{SubjectKind::None};
  TargetId target;
  DomainId domain;
  PoolId pool;
  ContractId contract;

  [[nodiscard]] static EvidenceSubject of(TargetId id) noexcept;
  [[nodiscard]] static EvidenceSubject of_domain(DomainId id) noexcept;
  [[nodiscard]] static EvidenceSubject of_pool(PoolId id) noexcept;
  [[nodiscard]] static EvidenceSubject of_contract(ContractId id) noexcept;
  [[nodiscard]] static EvidenceSubject global() noexcept;

  friend constexpr bool operator==(const EvidenceSubject&, const EvidenceSubject&) noexcept = default;
  friend constexpr auto operator<=>(const EvidenceSubject&, const EvidenceSubject&) noexcept = default;

  [[nodiscard]] bool valid() const noexcept { return kind != SubjectKind::None; }
};

[[nodiscard]] std::string to_string(const EvidenceSubject& subject);
[[nodiscard]] std::optional<EvidenceSubject> parse_evidence_subject(std::string_view text);

struct EvidenceKey {
  EvidenceKind kind{EvidenceKind::TargetHealth};
  EvidenceSubject subject;
  SourceId source;

  friend constexpr bool operator==(const EvidenceKey&, const EvidenceKey&) noexcept = default;
  friend constexpr auto operator<=>(const EvidenceKey&, const EvidenceKey&) noexcept = default;
};

[[nodiscard]] std::string to_string(const EvidenceKey& key);

struct EvidencePayload {
  Health health{Health::Unknown};
  std::uint32_t available_units{0};
  std::uint32_t total_units{0};
  /// Generic boolean outcome for check-style evidence (drain, restoration).
  bool result{false};
  /// Generic scalar (quorum members, degraded count).
  std::uint32_t count{0};

  friend constexpr bool operator==(const EvidencePayload&, const EvidencePayload&) noexcept = default;
};

struct EvidenceRecord {
  EvidenceId id;
  EvidenceKey key;
  /// Monotonic per key. A lower revision than the stored one is a replay and is
  /// rejected, never merged.
  Revision revision;
  Nanos observed_at{0};
  Nanos ttl{limits::kDefaultEvidenceTtlNanos};
  EvidenceClass klass{EvidenceClass::Volatile};
  EvidencePayload payload;
  ControllerIncarnation observed_by;
  /// Optional binding: the observation was produced by a specific attempt of a
  /// specific job generation.
  JobId job;
  AttemptId attempt;
  std::uint64_t digest{0};

  [[nodiscard]] bool valid() const noexcept;
};

enum class FreshnessState : std::uint8_t {
  Fresh = 0,
  Missing = 1,
  Expired = 2,
  FutureDated = 3,
  ForeignIncarnation = 4,
  AttemptMismatch = 5,
  JobMismatch = 6,
};

[[nodiscard]] const char* to_string(FreshnessState state) noexcept;

struct Freshness {
  FreshnessState state{FreshnessState::Missing};
  Nanos age{0};
  Nanos ttl{0};
  std::string detail;

  [[nodiscard]] bool fresh() const noexcept { return state == FreshnessState::Fresh; }
};

struct EvidenceQuery {
  EvidenceKey key;
  ControllerIncarnation current;
  Nanos now{0};
  /// Optional binding checks; an invalid id means "do not care".
  JobId job;
  AttemptId attempt;
};

/// Bounded store of the newest observation per (kind, subject, source).
/// Capacity is enforced: a full store accepts updates to known keys and evicts
/// the oldest expired record to admit a new key, otherwise it refuses.
class EvidenceStore {
 public:
  explicit EvidenceStore(std::size_t capacity = limits::kMaxEvidenceRecords);

  /// Rejects invalid keys, zero revisions, foreign/older nonces that would
  /// regress a newer observation, and duplicate ids.
  [[nodiscard]] Status observe(EvidenceRecord record);

  [[nodiscard]] const EvidenceRecord* find(const EvidenceKey& key) const noexcept;
  [[nodiscard]] Freshness evaluate(const EvidenceQuery& query) const;
  /// Number of distinct sources holding a fresh record for the subject.
  [[nodiscard]] std::size_t fresh_source_count(EvidenceKind kind, const EvidenceSubject& subject,
                                               const ControllerIncarnation& current,
                                               Nanos now) const;
  [[nodiscard]] std::vector<SourceId> fresh_sources(EvidenceKind kind,
                                                    const EvidenceSubject& subject,
                                                    const ControllerIncarnation& current,
                                                    Nanos now) const;
  /// All records for a subject and kind, ordered by source.
  [[nodiscard]] std::vector<const EvidenceRecord*> records_for(EvidenceKind kind,
                                                               const EvidenceSubject& subject) const;

  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t observations() const noexcept { return observations_; }
  [[nodiscard]] std::uint64_t rejections() const noexcept { return rejections_; }
  [[nodiscard]] std::uint64_t evictions() const noexcept { return evictions_; }
  [[nodiscard]] const std::map<EvidenceKey, EvidenceRecord>& records() const noexcept {
    return records_;
  }

  void clear();

 private:
  [[nodiscard]] bool evict_expired(Nanos now);

  std::map<EvidenceKey, EvidenceRecord> records_;
  std::size_t capacity_;
  std::uint64_t observations_{0};
  std::uint64_t rejections_{0};
  std::uint64_t evictions_{0};
};

}  // namespace mf
