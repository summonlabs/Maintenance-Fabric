// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// End-to-end walk through the maintenance lifecycle using only the public API.

#include <cstdio>

#include "examples/common.hpp"

using namespace mf;

int main() {
  const std::string directory = example::scratch_directory("basic-flow");
  ManualClock clock(1750000000LL * kNanosPerSecond);
  auto drain = std::make_shared<LocalDrainPort>();

  ControllerOptions options;
  options.store.journal_path = directory + "/journal.mfj";
  options.name = "example";
  RecoveryReport recovery;
  Result<std::unique_ptr<Controller>> opened =
      Controller::open(options, drain, clock, recovery);
  if (!opened.ok()) {
    std::fprintf(stderr, "cannot open the controller: %s\n",
                 format_error(opened.error()).c_str());
    return 1;
  }
  std::unique_ptr<Controller> controller = std::move(opened.value());

  if (const Status status = controller->set_topology(example::fabric()); !status.ok()) {
    std::fprintf(stderr, "%s\n", format_error(status.error()).c_str());
    return 1;
  }
  if (const Status status = controller->set_policy(example::policy_with(1)); !status.ok()) {
    std::fprintf(stderr, "%s\n", format_error(status.error()).c_str());
    return 1;
  }
  if (const Status status = example::publish_health(*controller, clock.now()); !status.ok()) {
    std::fprintf(stderr, "%s\n", format_error(status.error()).c_str());
    return 1;
  }

  MaintenanceRequest request;
  request.title = "replace optics on lag-1-1";
  request.reason = "scheduled optics replacement";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.priority = 1;
  request.requestor = requestor_from_name("example");
  Result<JobId> created = controller->propose(request, "example");
  if (!created.ok()) {
    std::fprintf(stderr, "%s\n", format_error(created.error()).c_str());
    return 1;
  }
  const JobId job = created.value();
  std::printf("proposed %s\n", to_string(job).c_str());

  if (const Status status = controller->approve(job, "example"); !status.ok()) {
    std::fprintf(stderr, "%s\n", format_error(status.error()).c_str());
    return 1;
  }

  for (int pass = 0; pass < 16; ++pass) {
    (void)controller->tick(clock.now());
    clock.advance(kNanosPerSecond);
    const MaintenanceJob* current = controller->state().jobs.find(job);
    if (current == nullptr) {
      return 1;
    }
    if (current->state == JobState::InMaintenance) {
      std::printf("in maintenance as %s (authority %s, drain %s)\n",
                  to_string(current->active_attempt).c_str(),
                  to_string(current->authority).c_str(), to_string(current->lease).c_str());
      break;
    }
    if (current->state == JobState::Blocked) {
      std::printf("blocked: %s -- %s\n", to_string(current->block), current->block_detail.c_str());
      return 1;
    }
  }

  const MaintenanceJob* started = controller->state().jobs.find(job);
  if (started == nullptr || started->state != JobState::InMaintenance) {
    std::fprintf(stderr, "job never started\n");
    return 1;
  }

  // The external maintenance executor reports completion for the current attempt.
  if (const Status status =
          controller->report_completion(job, started->active_attempt, true, "optics replaced");
      !status.ok()) {
    std::fprintf(stderr, "%s\n", format_error(status.error()).c_str());
    return 1;
  }
  for (int pass = 0; pass < 16 && !controller->state().jobs.find(job)->is_terminal(); ++pass) {
    (void)controller->tick(clock.now());
    clock.advance(kNanosPerSecond);
  }

  Result<Explanation> explanation = controller->explain(job, "post-run");
  if (!explanation.ok()) {
    std::fprintf(stderr, "%s\n", format_error(explanation.error()).c_str());
    return 1;
  }
  std::printf("\n%s", explanation.value().to_text().c_str());
  const MaintenanceJob* finished = controller->state().jobs.find(job);
  std::printf("\nfinal state: %s (verification passes %u)\n", to_string(finished->state),
              static_cast<unsigned>(finished->verification_passes));
  return finished->state == JobState::Complete ? 0 : 1;
}
