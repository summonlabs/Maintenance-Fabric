#pragma once

// The controller is the only component that drives a maintenance job through the
// lifecycle. It owns all mutable runtime state behind one mutex, never invokes an
// external port while holding that mutex, and never calls back into itself from
// beneath a lock.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/drain.hpp"
#include "mf/domain/job.hpp"
#include "mf/engine/arbitrator.hpp"
#include "mf/engine/explain.hpp"
#include "mf/engine/preconditions.hpp"
#include "mf/engine/readiness.hpp"
#include "mf/engine/scheduler.hpp"
#include "mf/store/store.hpp"

namespace mf {

struct ControllerOptions {
  StoreOptions store;
  ControllerId controller_id{ControllerId::from_u64(1)};
  std::uint64_t nonce{0};
  std::string name{"controller"};
};

struct ControllerStats {
  std::uint64_t ticks{0};
  std::uint64_t proposals{0};
  std::uint64_t approvals{0};
  std::uint64_t rearmings{0};
  std::uint64_t cancellations{0};
  std::uint64_t starts{0};
  std::uint64_t completions{0};
  std::uint64_t failures{0};
  std::uint64_t blocks{0};
  std::uint64_t unblocks{0};
  std::uint64_t drain_requests{0};
  std::uint64_t drain_failures{0};
  std::uint64_t drain_releases{0};
  std::uint64_t verifications_passed{0};
  std::uint64_t verifications_failed{0};
  std::uint64_t restorations{0};
  std::uint64_t restoration_failures{0};
  std::uint64_t quarantines{0};
  std::uint64_t reconciliations{0};
  std::uint64_t authority_revocations{0};
  std::uint64_t authority_reissues{0};
  std::uint64_t stale_attempt_rejections{0};
  std::uint64_t stale_generation_rejections{0};
  std::uint64_t stale_incarnation_rejections{0};
  std::uint64_t stale_evidence_rejections{0};
  std::uint64_t compactions{0};
};

/// Reasons that cannot clear themselves by retrying: the operator must change
/// policy or topology, or explicitly re-arm the job under a new generation.
[[nodiscard]] bool is_hard_block(BlockReason reason) noexcept;

class Controller {
 public:
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
  ~Controller();

  [[nodiscard]] static Result<std::unique_ptr<Controller>> open(const ControllerOptions& options,
                                                                DrainPortPtr drain,
                                                                const Clock& clock,
                                                                RecoveryReport& report);

  // --- configuration --------------------------------------------------------
  [[nodiscard]] Status set_topology(Topology topology);
  [[nodiscard]] Status set_policy(Policy policy);

  // --- request lifecycle ----------------------------------------------------
  [[nodiscard]] Result<JobId> propose(MaintenanceRequest request, std::string actor);
  [[nodiscard]] Status approve(JobId job, std::string actor);
  /// Bumps the job generation, discards every token issued under the previous
  /// generation and returns the job to the prerequisites stage. Refused once
  /// service has been removed.
  [[nodiscard]] Status rearm(JobId job, std::string actor);
  [[nodiscard]] Status cancel(JobId job, std::string actor, std::string reason);

  [[nodiscard]] Status tick();
  [[nodiscard]] Status tick(Nanos now);

  // --- external reports -----------------------------------------------------
  [[nodiscard]] Status observe(EvidenceRecord evidence);
  [[nodiscard]] Status report_completion(JobId job, const AttemptId& attempt, bool succeeded,
                                         std::string detail);
  [[nodiscard]] Status report_restoration(JobId job, const AttemptId& attempt, TargetId target,
                                          bool ok, std::string detail);
  [[nodiscard]] Status clear_quarantine(QuarantineId id, std::string actor,
                                        std::string justification);

  // --- inspection -----------------------------------------------------------
  [[nodiscard]] Result<JobSnapshot> status(JobId job) const;
  [[nodiscard]] std::vector<JobSnapshot> list() const;
  [[nodiscard]] Result<Explanation> explain(JobId job, std::string action) const;
  [[nodiscard]] ConflictReport conflicts() const;
  /// The most recent arbitration pass, for inspection and explanation.
  [[nodiscard]] ArbitrationReport last_arbitration() const;
  /// Dry-run admission check: the precondition set a start would be decided on.
  [[nodiscard]] Result<PreconditionSet> admission_check(JobId job) const;
  [[nodiscard]] std::vector<QuarantineRecord> quarantines() const;
  [[nodiscard]] const PersistedState& state() const noexcept { return store_->state(); }
  [[nodiscard]] ControllerStats stats() const;
  [[nodiscard]] ControllerIncarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return store_->recovery(); }
  [[nodiscard]] const Policy& policy() const noexcept { return policy_; }
  [[nodiscard]] const Topology& topology() const noexcept { return topology_; }

  [[nodiscard]] Status shutdown();
  [[nodiscard]] bool shutting_down() const;

 private:
  Controller(std::unique_ptr<Store> store, DrainPortPtr drain, const Clock& clock,
             ControllerOptions options);

  struct DrainAction {
    enum class Kind : std::uint8_t { Request, Release, Query };
    Kind kind{Kind::Request};
    JobId job;
    GenerationId generation;
    AttemptId attempt;
    DrainLeaseId lease;
    std::vector<TargetId> targets;
    Nanos lease_duration{0};
    std::string reason;
  };

  struct DrainOutcome {
    DrainAction action;
    DrainResponse response;
  };

  struct Evaluation {
    ReadinessView readiness;
    CapacityImpact impact;
    PreconditionSet preconditions;
  };

  struct PassContext {
    Nanos now{0};
    const ReadinessView* readiness{nullptr};
    std::vector<DrainAction>* actions{nullptr};
  };

  [[nodiscard]] Evaluation evaluate_locked(const MaintenanceJob& job, const ReadinessView& readiness,
                                           bool check_authority, bool check_drain, Nanos now) const;
  [[nodiscard]] Status commit_job_locked(MaintenanceJob job, Nanos now);
  [[nodiscard]] bool transition_locked(MaintenanceJob& job, JobState to, BlockReason reason,
                                       std::string detail, Nanos now, PassContext& context);
  void new_attempt_locked(MaintenanceJob& job, Nanos now) const;
  [[nodiscard]] bool drive_locked(JobId id, PassContext& context);
  [[nodiscard]] bool drive_validated_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_prerequisites_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_ready_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_blocked_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_in_maintenance_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_verifying_locked(MaintenanceJob& job, PassContext& context);
  [[nodiscard]] bool drive_restoring_locked(MaintenanceJob& job, PassContext& context);
  void drain_all_locked(PassContext& context, std::vector<DrainAction>& actions);
  void apply_outcomes_locked(const std::vector<DrainOutcome>& outcomes, Nanos now);
  void perform_action(const DrainAction& action, std::vector<DrainOutcome>& outcomes);
  [[nodiscard]] Status reconcile_locked(Nanos now);
  [[nodiscard]] Status write_evidence_locked(EvidenceRecord record);
  void quarantine_targets_locked(const MaintenanceJob& job, BlockReason reason, std::string detail,
                                 Nanos now);
  void release_resources_locked(MaintenanceJob& job, Nanos now, PassContext& context);
  [[nodiscard]] bool completion_confirmed_locked(const MaintenanceJob& job, Nanos now,
                                                 bool& any_failed) const;
  /// Grants maintenance authority in the arbitrator's deterministic order. This
  /// is the only path that creates a reservation, so two jobs can never consume
  /// the same correlated tolerance budget independently.
  void arbitrate_locked(PassContext& context, std::vector<DrainAction>& actions);
  [[nodiscard]] EvidenceRecord make_evidence_locked(EvidenceKind kind,
                                                    const EvidenceSubject& subject, SourceId source,
                                                    Revision revision, Nanos now, Nanos ttl,
                                                    const EvidencePayload& payload, JobId job,
                                                    const AttemptId& attempt);

  mutable std::mutex mu_;
  std::unique_ptr<Store> store_;
  DrainPortPtr drain_;
  const Clock* clock_;
  ControllerOptions options_;
  ControllerIncarnation incarnation_;
  Policy policy_;
  Topology topology_;
  std::uint64_t evidence_revision_{0};
  std::set<JobId> drain_inflight_;
  std::set<DrainLeaseId> queued_releases_;
  std::vector<DrainAction> deferred_releases_;
  ArbitrationReport last_arbitration_;
  ControllerStats stats_;
  bool stopping_{false};
};

}  // namespace mf
