#pragma once

// Durable runtime state. The store owns the journal and is the only component
// allowed to mutate persisted state: every mutation is appended to the journal
// first and applied to memory second, so a crash between the two can only lose
// work the runtime had not yet acknowledged.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/domain/authority.hpp"
#include "mf/domain/drain.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/store/journal.hpp"
#include "mf/store/records.hpp"
#include "mf/version.hpp"

namespace mf {

struct PersistedState {
  Topology topology;
  Policy policy;
  JobTable jobs;
  EvidenceStore evidence;
  AuthorityLedger ledger;
  QuarantineTable quarantines;
  std::map<DrainLeaseId, DrainLease> leases;
  ControllerIncarnation last_incarnation;
  BootEpoch boot_epoch;
  Revision revision;
};

struct StoreOptions {
  std::string journal_path;
  bool create_if_missing{true};
  std::size_t evidence_capacity{limits::kMaxEvidenceRecords};
  Nanos authority_lease{limits::kDefaultAuthorityLeaseNanos};
  std::uint64_t compaction_threshold_records{limits::kJournalCompactionThresholdRecords};
};

struct RecoveryReport {
  bool journal_created{false};
  std::uint32_t format_version{kStateFormatVersion};
  std::uint64_t frames_replayed{0};
  std::uint64_t records_replayed{0};
  std::uint64_t discarded_records{0};
  std::uint64_t bytes_recovered{0};
  std::uint64_t truncated_bytes{0};
  bool truncated{false};
  BootEpoch previous_boot_epoch;
  BootEpoch boot_epoch;
  std::uint64_t jobs_recovered{0};
  std::uint64_t reservations_recovered{0};
  std::uint64_t leases_recovered{0};
  std::uint64_t quarantines_recovered{0};
  std::uint64_t evidence_recovered{0};
  std::string detail;

  [[nodiscard]] std::string to_text() const;
};

class Store {
 public:
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  [[nodiscard]] static Result<std::unique_ptr<Store>> open(const StoreOptions& options,
                                                           RecoveryReport& report);

  [[nodiscard]] const PersistedState& state() const noexcept { return state_; }
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return report_; }
  [[nodiscard]] ControllerIncarnation incarnation() const noexcept {
    return state_.last_incarnation;
  }

  /// Claims this process's incarnation: bumps the boot epoch, revokes nothing by
  /// itself, and durably records the new epoch before returning.
  [[nodiscard]] Result<ControllerIncarnation> begin_incarnation(ControllerId controller,
                                                                std::uint64_t nonce);

  [[nodiscard]] Status commit_topology(Topology topology);
  [[nodiscard]] Status commit_policy(Policy policy);
  /// Allocates the job identity, inserts the record and commits it in one step.
  [[nodiscard]] Result<JobId> create_job(MaintenanceJob job);
  /// Monotonic evidence identity for the running incarnation.
  [[nodiscard]] EvidenceId allocate_evidence_id();
  /// Allocates a quarantine identity and commits the record.
  [[nodiscard]] Result<QuarantineId> quarantine_target(TargetId target, JobId job,
                                                       GenerationId generation, BlockReason reason,
                                                       std::string detail, Nanos now,
                                                       const ControllerIncarnation& by);
  [[nodiscard]] Status commit_job(MaintenanceJob job);
  [[nodiscard]] Status commit_jobs(const std::vector<MaintenanceJob>& jobs);
  [[nodiscard]] Status commit_job_erased(JobId job);
  [[nodiscard]] Status observe_evidence(EvidenceRecord evidence);
  [[nodiscard]] Result<AuthorityId> reserve_authority(AuthorityReservation reservation);
  /// Installs a reservation exactly as produced by arbitration, preserving its
  /// identity, so the granted id and the durable record agree.
  [[nodiscard]] Status insert_authority(const AuthorityReservation& reservation);
  [[nodiscard]] Status release_authority(AuthorityId id, const ControllerIncarnation& current);
  [[nodiscard]] Status renew_authority(AuthorityId id, const AttemptId& attempt,
                                       const ControllerIncarnation& current, Nanos extend, Nanos now);
  [[nodiscard]] std::size_t expire_authority(Nanos now);
  [[nodiscard]] std::vector<AuthorityReservation> fence_authority(
      const ControllerIncarnation& current);
  [[nodiscard]] Status commit_lease(DrainLease lease);
  [[nodiscard]] Status commit_lease_erased(DrainLeaseId id);
  [[nodiscard]] Status commit_quarantine(QuarantineRecord record);

  [[nodiscard]] Status flush();
  [[nodiscard]] Status compact(Nanos now);
  [[nodiscard]] bool should_compact() const noexcept;
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_.bytes(); }
  [[nodiscard]] std::uint64_t journal_records() const noexcept { return journal_.records(); }
  [[nodiscard]] std::uint64_t compactions() const noexcept { return compactions_; }

 private:
  Store(Journal journal, StoreOptions options);

  [[nodiscard]] Status apply(const Record& record, bool strict);
  [[nodiscard]] Status append_and_apply(const std::vector<Record>& records);

  Journal journal_;
  StoreOptions options_;
  PersistedState state_;
  RecoveryReport report_;
  std::uint64_t compactions_{0};
  std::uint64_t evidence_sequence_{0};
};

}  // namespace mf
