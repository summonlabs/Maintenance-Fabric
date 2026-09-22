// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// In-process restart: the controller is destroyed and reopened against the same
// journal while a scope is out of service, showing incarnation fencing,
// reconciliation and the mandatory re-verification.

#include <cstdio>

#include "examples/common.hpp"

using namespace mf;

int main() {
  const std::string directory = example::scratch_directory("restart-recovery");
  const std::string journal = directory + "/journal.mfj";
  ManualClock clock(1750000000LL * kNanosPerSecond);
  auto drain = std::make_shared<LocalDrainPort>();

  JobId job;
  AttemptId first_attempt;
  ControllerIncarnation first_incarnation;
  {
    ControllerOptions options;
    options.store.journal_path = journal;
    options.nonce = 1;
    RecoveryReport recovery;
    Result<std::unique_ptr<Controller>> opened = Controller::open(options, drain, clock, recovery);
    if (!opened.ok()) {
      std::fprintf(stderr, "%s\n", format_error(opened.error()).c_str());
      return 1;
    }
    std::unique_ptr<Controller> controller = std::move(opened.value());
    first_incarnation = controller->incarnation();
    (void)controller->set_topology(example::fabric());
    (void)controller->set_policy(example::policy_with(1));
    (void)example::publish_health(*controller, clock.now());
    MaintenanceRequest request;
    request.title = "long maintenance";
    request.targets = {TargetId{TargetKind::Link, 1}};
    request.requestor = requestor_from_name("example");
    Result<JobId> created = controller->propose(request, "example");
    if (!created.ok()) {
      return 1;
    }
    job = created.value();
    (void)controller->approve(job, "example");
    for (int pass = 0; pass < 16; ++pass) {
      (void)controller->tick(clock.now());
      clock.advance(kNanosPerSecond);
      const MaintenanceJob* current = controller->state().jobs.find(job);
      if (current != nullptr && current->state == JobState::InMaintenance) {
        first_attempt = current->active_attempt;
        break;
      }
    }
    std::printf("first incarnation %s started %s as %s\n", to_string(first_incarnation).c_str(),
                to_string(job).c_str(), to_string(first_attempt).c_str());
    (void)controller->shutdown();
  }

  ControllerOptions options;
  options.store.journal_path = journal;
  options.nonce = 2;
  RecoveryReport recovery;
  Result<std::unique_ptr<Controller>> opened = Controller::open(options, drain, clock, recovery);
  if (!opened.ok()) {
    std::fprintf(stderr, "%s\n", format_error(opened.error()).c_str());
    return 1;
  }
  std::unique_ptr<Controller> controller = std::move(opened.value());
  std::printf("reopened as %s after %s\n", to_string(controller->incarnation()).c_str(),
              recovery.to_text().c_str());

  const Status fenced =
      controller->report_completion(job, first_attempt, true, "reported by the vanished process");
  std::printf("old attempt report: %s\n", format_error(fenced.error()).c_str());

  const MaintenanceJob* recovered = controller->state().jobs.find(job);
  const BlockReason recovered_block = recovered->block;
  const bool authority_retained = recovered->authority.valid();
  std::printf("reconciled state: %s (block %s), authority %s retained\n",
              to_string(recovered->state), to_string(recovered_block),
              authority_retained ? "yes" : "no");

  (void)example::publish_health(*controller, clock.now());
  for (int pass = 0; pass < 32 && !controller->state().jobs.find(job)->is_terminal(); ++pass) {
    (void)controller->tick(clock.now());
    clock.advance(kNanosPerSecond);
  }
  const MaintenanceJob* finished = controller->state().jobs.find(job);
  std::printf("final state: %s\n", to_string(finished->state));

  const bool ok = recovered_block == BlockReason::ReconciledAfterRestart &&
                  authority_retained && !fenced.ok() && finished->state == JobState::Complete;
  return ok ? 0 : 1;
}
