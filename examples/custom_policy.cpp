// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates that redundancy tolerance is generic policy: the same topology
// and the same request are admissible under N+2 and refused under N+1 once a
// member is already down.

#include <cstdio>
#include <vector>

#include "examples/common.hpp"

using namespace mf;

namespace {

struct Outcome {
  bool started{false};
  std::string state;
  std::string reason;
};

Outcome attempt(std::uint32_t tolerated_losses, const std::vector<std::uint32_t>& down_links) {
  const std::string directory =
      example::scratch_directory("custom-policy-" + std::to_string(tolerated_losses));
  ManualClock clock(1750000000LL * kNanosPerSecond);
  auto drain = std::make_shared<LocalDrainPort>();
  ControllerOptions options;
  options.store.journal_path = directory + "/journal.mfj";
  RecoveryReport recovery;
  Result<std::unique_ptr<Controller>> opened = Controller::open(options, drain, clock, recovery);
  if (!opened.ok()) {
    return Outcome{false, "open-failed", format_error(opened.error())};
  }
  std::unique_ptr<Controller> controller = std::move(opened.value());
  (void)controller->set_topology(example::fabric());
  (void)controller->set_policy(example::policy_with(tolerated_losses));
  (void)example::publish_health(*controller, clock.now());
  // Pool 1 holds every even-numbered index of the fabric's first link row, so
  // the degraded members are chosen from the same pool as the target.
  for (const std::uint32_t index : down_links) {
    EvidenceRecord evidence;
    evidence.key.kind = EvidenceKind::TargetHealth;
    evidence.key.subject = EvidenceSubject::of(TargetId{TargetKind::Link, index});
    evidence.key.source = SourceId::from_u64(1);
    evidence.revision = Revision::from_u64(example::next_evidence_revision());
    evidence.observed_at = clock.now();
    evidence.ttl = 100 * kNanosPerSecond;
    evidence.observed_by = controller->incarnation();
    evidence.payload.health = Health::Down;
    (void)controller->observe(evidence);
  }
  MaintenanceRequest request;
  request.title = "policy probe";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.requestor = requestor_from_name("example");
  Result<JobId> created = controller->propose(request, "example");
  if (!created.ok()) {
    return Outcome{false, "propose-failed", format_error(created.error())};
  }
  (void)controller->approve(created.value(), "example");
  for (int pass = 0; pass < 12; ++pass) {
    (void)controller->tick(clock.now());
    clock.advance(kNanosPerSecond);
    const MaintenanceJob* job = controller->state().jobs.find(created.value());
    if (job == nullptr) {
      break;
    }
    if (job->state == JobState::InMaintenance) {
      return Outcome{true, "in-maintenance", ""};
    }
    if (job->state == JobState::Blocked) {
      return Outcome{false, "blocked", job->block_detail};
    }
  }
  return Outcome{false, "timeout", ""};
}

}  // namespace

int main() {
  // link:1 is the target throughout; link:3 and link:5 are the other members of
  // the same redundancy pool, in different racks.
  const Outcome n_plus_one = attempt(1, {});
  const Outcome n_plus_two = attempt(2, {});
  const Outcome n_plus_one_one_down = attempt(1, {3});
  const Outcome n_plus_two_one_down = attempt(2, {3});
  const Outcome n_plus_one_two_down = attempt(1, {3, 5});

  const auto show = [](const char* label, const Outcome& outcome) {
    std::printf("%-28s: %-7s %s\n", label, outcome.started ? "START" : "BLOCKED",
                outcome.reason.c_str());
  };
  show("N+1, healthy pool", n_plus_one);
  show("N+2, healthy pool", n_plus_two);
  show("N+1, one member down", n_plus_one_one_down);
  show("N+2, one member down", n_plus_two_one_down);
  show("N+1, two members down", n_plus_one_two_down);

  // A four-member pool satisfies N+1 after a single loss, and N+2 only while
  // nothing is degraded; two losses exhaust the N+1 budget entirely.
  const bool ok = n_plus_one.started && n_plus_two.started && n_plus_one_one_down.started &&
                  !n_plus_two_one_down.started && !n_plus_one_two_down.started;
  return ok ? 0 : 1;
}
