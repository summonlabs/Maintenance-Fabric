#pragma once

// Maintenance job model. A job is a request plus its generation, attempt
// history, authority and drain bindings, and the lifecycle position it reached.
// Every mutation bumps the job revision; attempts carry a full AttemptId so a
// result reported for an old generation can never complete a newer one.

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
#include "mf/domain/authority.hpp"
#include "mf/domain/drain.hpp"
#include "mf/domain/identity.hpp"
#include "mf/domain/lifecycle.hpp"
#include "mf/domain/window.hpp"

namespace mf {

/// Why a job is not progressing. Distinct reasons matter: the CLI, the
/// explanation digest and the property tests all discriminate on them.
enum class BlockReason : std::uint16_t {
  None = 0,
  TargetUnknown = 1,
  TargetNotMaintainable = 2,
  EvidenceMissingOrStale = 3,
  RedundancyViolation = 4,
  FailureDomainLimit = 5,
  ContractViolation = 6,
  CapacityHeadroom = 7,
  ConflictingMaintenance = 8,
  DependencyUnmet = 9,
  WindowUnavailable = 10,
  AuthorityDenied = 11,
  DrainFailed = 12,
  DrainLeaseLost = 13,
  AwaitingCompletionReport = 14,
  VerificationFailed = 15,
  RestorationFailed = 16,
  ReconciledAfterRestart = 17,
  ControlPlaneUnsafe = 18,
  PolicyDenied = 19,
  OperatorHold = 20,
  Overflow = 21,
};

inline constexpr std::size_t kBlockReasonCount = 22;

[[nodiscard]] const char* to_string(BlockReason reason) noexcept;
[[nodiscard]] std::optional<BlockReason> parse_block_reason(std::string_view text) noexcept;
/// True when the reason names a safety invariant rather than an operational
/// hiccup. A job blocked for a safety reason must never be auto-advanced.
[[nodiscard]] bool is_safety_block(BlockReason reason) noexcept;

enum class AttemptOutcome : std::uint8_t {
  Running = 0,
  Succeeded = 1,
  Failed = 2,
  Fenced = 3,
  Cancelled = 4,
};

[[nodiscard]] const char* to_string(AttemptOutcome outcome) noexcept;
[[nodiscard]] std::optional<AttemptOutcome> parse_attempt_outcome(std::string_view text) noexcept;

struct MaintenanceRequest {
  std::string title;
  std::string reason;
  std::vector<TargetId> targets;
  std::vector<JobId> depends_on;
  /// Empty means the request accepts any window that applies to its targets.
  std::vector<WindowId> window_filter;
  std::uint32_t priority{0};
  Nanos estimated_duration{0};
  bool requires_drain{true};
  RequestorId requestor;
};

struct AttemptRecord {
  AttemptId id;
  Nanos started_at{0};
  Nanos ended_at{0};
  AttemptOutcome outcome{AttemptOutcome::Running};
  std::string detail;
};

struct HistoryEntry {
  Nanos at{0};
  JobState from{JobState::Proposed};
  JobState to{JobState::Proposed};
  BlockReason reason{BlockReason::None};
  std::string detail;
  ControllerIncarnation by;
};

struct JobSnapshot {
  JobId id;
  GenerationId generation;
  Revision revision;
  JobState state{JobState::Proposed};
  BlockReason block{BlockReason::None};
  std::string block_detail;
  std::vector<TargetId> targets;
  std::uint32_t priority{0};
  bool service_removed{false};
  bool overrun{false};
  AuthorityId authority;
  DrainLeaseId lease;
  AttemptId active_attempt;
  Nanos created_at{0};
  Nanos updated_at{0};
};

struct MaintenanceJob {
  JobId id;
  GenerationId generation{GenerationId::from_u64(1)};
  Revision revision;

  std::string title;
  std::string reason;
  std::vector<TargetId> targets;
  std::vector<TargetId> removal_set;
  std::vector<JobId> depends_on;
  std::vector<WindowId> window_filter;
  std::uint32_t priority{0};
  Nanos estimated_duration{0};
  bool requires_drain{true};
  RequestorId requestor;

  JobState state{JobState::Proposed};
  BlockReason block{BlockReason::None};
  std::string block_detail;
  std::string last_detail;

  Revision policy_revision;
  Revision topology_revision;
  std::uint64_t policy_digest{0};
  std::uint64_t topology_digest{0};

  AuthorityId authority;
  DrainLeaseId lease;
  AttemptId active_attempt;
  AttemptOrdinal next_attempt{AttemptOrdinal::from_u64(1)};
  std::vector<AttemptRecord> attempts;

  std::vector<HistoryEntry> history;
  std::uint32_t history_dropped{0};

  bool service_removed{false};
  bool overrun{false};
  bool approved{false};
  std::string approved_by;

  Nanos created_at{0};
  Nanos updated_at{0};
  Nanos started_at{0};
  Nanos completed_at{0};

  Nanos preconditions_at{0};
  std::uint64_t preconditions_digest{0};
  Revision preconditions_policy_revision;
  Revision preconditions_topology_revision;
  bool preconditions_satisfied{false};

  std::uint32_t verification_passes{0};
  std::uint32_t verification_failures{0};
  std::uint32_t restoration_failures{0};
  std::uint32_t drain_failures{0};
  /// Start of the current verification or restoration round. A round that lasts
  /// longer than the job's own estimated duration consumes one retry.
  Nanos verification_started_at{0};
  Nanos restoration_started_at{0};
  /// The external maintenance action itself reported failure. Service must
  /// still be restored; the job then terminates as failed rather than complete.
  bool maintenance_failed{false};
  /// The policy that governs the window explicitly sanctioned an overrun.
  bool window_extended{false};

  WindowId window;
  OnWindowClose on_close{OnWindowClose::FinishActiveStep};
  Nanos window_closes_at{0};

  ControllerIncarnation owner;

  [[nodiscard]] bool is_terminal() const noexcept { return mf::is_terminal(state); }
  [[nodiscard]] bool holds_resources() const noexcept { return mf::holds_resources(state); }
  [[nodiscard]] std::optional<AttemptRecord> active_attempt_record() const;
  [[nodiscard]] std::uint64_t digest() const;
  [[nodiscard]] JobSnapshot snapshot() const;
  [[nodiscard]] std::string to_text() const;

  /// Appends a history entry, dropping the oldest entries beyond
  /// limits::kMaxJobHistoryEntries and counting the loss.
  void push_history(HistoryEntry entry);
};

class JobTable {
 public:
  [[nodiscard]] Status insert(MaintenanceJob job);
  /// Fenced update: the replacement must be exactly one revision newer than
  /// the stored job, so a writer that read a stale revision is rejected.
  [[nodiscard]] Status put(MaintenanceJob job);
  /// Recovery path: installs a record exactly as persisted, without revision
  /// fencing, and never moves the allocated id backwards.
  [[nodiscard]] Status adopt(MaintenanceJob job);
  [[nodiscard]] Status erase(JobId id);
  [[nodiscard]] MaintenanceJob* find(JobId id) noexcept;
  [[nodiscard]] const MaintenanceJob* find(JobId id) const noexcept;
  [[nodiscard]] const std::map<JobId, MaintenanceJob>& jobs() const noexcept { return jobs_; }
  [[nodiscard]] std::size_t size() const noexcept { return jobs_.size(); }
  [[nodiscard]] JobId allocate_id();
  void set_next_id(JobId next) noexcept { next_id_ = next; }
  [[nodiscard]] JobId next_id() const noexcept { return next_id_; }
  [[nodiscard]] std::uint64_t digest() const;
  void clear();

 private:
  std::map<JobId, MaintenanceJob> jobs_;
  JobId next_id_{JobId::from_u64(1)};
};

/// A target that may not be returned to normal service until an operator or a
/// successful restoration proves it is whole. Quarantine is created by failed
/// restoration, by a lost drain lease, or by reconciliation after a restart with
/// unproven resource state.
struct QuarantineRecord {
  QuarantineId id;
  TargetId target;
  JobId job;
  GenerationId generation;
  BlockReason reason{BlockReason::None};
  std::string detail;
  Nanos created_at{0};
  ControllerIncarnation created_by;
  bool cleared{false};
  Nanos cleared_at{0};
  std::string cleared_by;
  std::uint64_t digest{0};
};

/// Deterministic table of quarantined targets, keyed by quarantine id.
class QuarantineTable {
 public:
  [[nodiscard]] Status insert(QuarantineRecord record);
  [[nodiscard]] Status put(QuarantineRecord record);
  [[nodiscard]] Status adopt(QuarantineRecord record);
  [[nodiscard]] Status erase(QuarantineId id);
  [[nodiscard]] const QuarantineRecord* find(QuarantineId id) const noexcept;
  /// Newest open quarantine covering \p target, if any.
  [[nodiscard]] const QuarantineRecord* find_open_for_target(TargetId target) const noexcept;
  [[nodiscard]] const std::map<QuarantineId, QuarantineRecord>& records() const noexcept {
    return records_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] QuarantineId allocate_id();
  void set_next_id(QuarantineId next) noexcept { next_id_ = next; }
  [[nodiscard]] QuarantineId next_id() const noexcept { return next_id_; }
  void clear();

 private:
  std::map<QuarantineId, QuarantineRecord> records_;
  QuarantineId next_id_{QuarantineId::from_u64(1)};
};

[[nodiscard]] std::uint64_t quarantine_digest(const QuarantineRecord& record);

/// Structural validation of a request against limits and target existence is
/// performed by the controller; this validates the request shape only.
[[nodiscard]] Status validate_request_shape(const MaintenanceRequest& request);

}  // namespace mf
