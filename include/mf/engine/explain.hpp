#pragma once

// Deterministic explanations. A decision must be inspectable: the evidence it
// consumed, the policy that governed it, the action selected, the alternatives
// rejected, and the generation/authority/revisions under which it was made.
// The same inputs always render the same text and the same digest.

#include <cstdint>
#include <string>
#include <vector>

#include "mf/domain/authority.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/engine/impact.hpp"
#include "mf/engine/preconditions.hpp"
#include "mf/engine/readiness.hpp"

namespace mf {

struct ExplanationLine {
  std::string code;
  std::string text;
};

struct Explanation {
  std::string subject;
  std::string action;
  std::string outcome;
  JobSnapshot job;
  std::vector<ExplanationLine> evidence;
  std::vector<ExplanationLine> policy;
  std::vector<ExplanationLine> lifecycle;
  std::vector<ExplanationLine> rejected;
  Nanos at{0};
  ControllerIncarnation by;
  Revision policy_revision;
  Revision topology_revision;
  std::uint64_t policy_digest{0};
  std::uint64_t topology_digest{0};
  std::uint64_t readiness_digest{0};
  std::uint64_t impact_digest{0};
  std::uint64_t preconditions_digest{0};
  std::uint64_t digest{0};

  [[nodiscard]] std::string to_text() const;
};

struct ExplainContext {
  const MaintenanceJob* job{nullptr};
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  const EvidenceStore* evidence{nullptr};
  const AuthorityLedger* ledger{nullptr};
  const JobTable* jobs{nullptr};
  const ReadinessView* readiness{nullptr};
  const PreconditionSet* preconditions{nullptr};
  const CapacityImpact* impact{nullptr};
  ControllerIncarnation current;
  Nanos now{0};
  /// "status", "start", "admission", ...
  std::string action{"status"};
};

[[nodiscard]] Explanation explain_job(const ExplainContext& context);

struct ConflictEntry {
  JobId first;
  JobId second;
  TargetId shared;
  std::string kind;
  std::string detail;
};

struct ConflictReport {
  std::vector<ConflictEntry> conflicts;
  std::vector<ExplanationLine> reservations;
  std::vector<ExplanationLine> blocked;
  std::vector<ExplanationLine> queued;
  Nanos at{0};
  std::uint64_t digest{0};

  [[nodiscard]] std::string to_text() const;
};

struct ConflictContext {
  const JobTable* jobs{nullptr};
  const AuthorityLedger* ledger{nullptr};
  const Topology* topology{nullptr};
  const Policy* policy{nullptr};
  Nanos now{0};
};

[[nodiscard]] ConflictReport inspect_conflicts(const ConflictContext& context);

}  // namespace mf
