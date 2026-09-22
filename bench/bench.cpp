// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmarks measure COMPLETED work: job lifecycles that reached a terminal
// state, arbitrations that produced a decision, records that were durably
// committed and then replayed, and freshness evaluations that returned a
// verdict. Submission latency alone is never reported as throughput.

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "bench/common.hpp"
#include "mf/drain/local.hpp"
#include "mf/store/store.hpp"

using namespace mf;

namespace {

using WallClock = std::chrono::steady_clock;

double seconds_since(WallClock::time_point start) {
  return std::chrono::duration<double>(WallClock::now() - start).count();
}

void report(const char* name, std::uint64_t completed, double seconds) {
  const double per_second = seconds > 0 ? static_cast<double>(completed) / seconds : 0.0;
  std::printf("%-42s completed=%-9llu  %10.1f ops/s  %8.3f us/op\n", name,
              static_cast<unsigned long long>(completed), per_second,
              per_second > 0 ? 1e6 / per_second : 0.0);
}

std::string scratch(const std::string& name) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / ("mf-bench-" + name);
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t jobs = 400;
  std::uint64_t arbitrations = 400;
  std::uint64_t records = 20000;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--jobs=", 0) == 0) {
      jobs = std::strtoull(arg.c_str() + 7, nullptr, 10);
    } else if (arg.rfind("--arbitrations=", 0) == 0) {
      arbitrations = std::strtoull(arg.c_str() + 15, nullptr, 10);
    } else if (arg.rfind("--records=", 0) == 0) {
      records = std::strtoull(arg.c_str() + 10, nullptr, 10);
    }
  }

  std::printf("Maintenance Fabric %s benchmark -- completed work only\n\n",
              std::string(kProductVersion).c_str());

  // --- completed job lifecycles ---------------------------------------------
  {
    const std::string directory = scratch("lifecycle");
    ManualClock clock(1750000000LL * kNanosPerSecond);
    auto drain = std::make_shared<LocalDrainPort>();
    ControllerOptions options;
    options.store.journal_path = directory + "/journal.mfj";
    RecoveryReport recovery;
    Result<std::unique_ptr<Controller>> opened = Controller::open(options, drain, clock, recovery);
    if (!opened.ok()) {
      std::fprintf(stderr, "%s\n", format_error(opened.error()).c_str());
      return 1;
    }
    std::unique_ptr<Controller> controller = std::move(opened.value());
    // One target per job so the measurement is admission and lifecycle work
    // rather than contention over a shared scope.
    const std::size_t switches = static_cast<std::size_t>(jobs > 512 ? 512 : jobs);
    const Status topology_status = controller->set_topology(bench::wide_fabric(switches));
    if (!topology_status.ok()) {
      std::fprintf(stderr, "benchmark fixture rejected: %s\n",
                   format_error(topology_status.error()).c_str());
      return 1;
    }
    const Status policy_status = controller->set_policy(bench::wide_policy(3));
    if (!policy_status.ok()) {
      std::fprintf(stderr, "benchmark policy rejected: %s\n",
                   format_error(policy_status.error()).c_str());
      return 1;
    }

    // Evidence is published once and kept fresh by the policy TTL, so the
    // measurement is the decision path rather than the journal fsync rate.
    (void)bench::publish_health(*controller, clock.now());
    const WallClock::time_point start = WallClock::now();
    std::uint64_t completed = 0;
    for (std::uint64_t i = 0; i < jobs; ++i) {
      MaintenanceRequest request;
      request.title = "bench job " + std::to_string(i);
      request.targets = {bench::target_for(i, switches)};
      request.priority = 1;
      request.requires_drain = true;
      request.requestor = requestor_from_name("bench");
      Result<JobId> created = controller->propose(request, "bench");
      if (!created.ok()) {
        continue;
      }
      (void)controller->approve(created.value(), "bench");
      for (int pass = 0; pass < 40; ++pass) {
        (void)controller->tick(clock.now());
        clock.advance(kNanosPerSecond);
        const MaintenanceJob* job = controller->state().jobs.find(created.value());
        if (job == nullptr) {
          break;
        }
        if (job->state == JobState::InMaintenance && job->active_attempt.valid()) {
          (void)controller->report_completion(created.value(), job->active_attempt, true, "bench");
        }
        if (job->is_terminal()) {
          if (job->state == JobState::Complete) {
            ++completed;
          }
          break;
        }
      }
    }
    const double elapsed = seconds_since(start);
    report("job lifecycles completed", completed, elapsed);
    // Report where the remaining jobs ended so a regression is visible rather
    // than hidden behind a throughput number.
    std::map<std::string, std::uint64_t> histogram;
    for (const JobSnapshot& snapshot : controller->list()) {
      if (snapshot.state == JobState::Complete) {
        continue;
      }
      std::string key = to_string(snapshot.state);
      if (snapshot.block != BlockReason::None) {
        key += "/";
        key += to_string(snapshot.block);
      }
      ++histogram[key];
    }
    for (const auto& [key, count] : histogram) {
      std::printf("%-42s %llu job(s)\n", ("  unfinished: " + key).c_str(),
                  static_cast<unsigned long long>(count));
    }
  }

  // --- arbitration passes ----------------------------------------------------
  {
    const std::string directory = scratch("arbitration");
    ManualClock clock(1750000000LL * kNanosPerSecond);
    auto drain = std::make_shared<LocalDrainPort>();
    ControllerOptions options;
    options.store.journal_path = directory + "/journal.mfj";
    RecoveryReport recovery;
    Result<std::unique_ptr<Controller>> opened = Controller::open(options, drain, clock, recovery);
    if (!opened.ok()) {
      return 1;
    }
    std::unique_ptr<Controller> controller = std::move(opened.value());
    const std::size_t switches = 128;
    (void)controller->set_topology(bench::wide_fabric(switches));
    if (const Status status = controller->set_policy(bench::wide_policy(2)); !status.ok()) {
      std::fprintf(stderr, "benchmark policy rejected: %s\n",
                   format_error(status.error()).c_str());
      return 1;
    }

    const WallClock::time_point start = WallClock::now();
    std::uint64_t decisions = 0;
    (void)bench::publish_health(*controller, clock.now());
    for (std::uint64_t i = 0; i < arbitrations; ++i) {
      MaintenanceRequest request;
      request.title = "arb " + std::to_string(i);
      request.targets = {bench::target_for(i, switches)};
      request.priority = 1;
      request.requestor = requestor_from_name("bench");
      Result<JobId> created = controller->propose(request, "bench");
      if (!created.ok()) {
        continue;
      }
      (void)controller->approve(created.value(), "bench");
      (void)controller->tick(clock.now());
      clock.advance(30 * kNanosPerSecond);
      decisions += controller->last_arbitration().decisions.size();
      // Keep the table bounded: retire completed and terminal jobs.
      for (const JobSnapshot& snapshot : controller->list()) {
        if (is_terminal(snapshot.state) && controller->state().jobs.size() > 64) {
          break;
        }
      }
    }
    report("arbitration decisions", decisions, seconds_since(start));
  }

  // --- durable commit and replay --------------------------------------------
  {
    const std::string directory = scratch("journal");
    const std::string path = directory + "/journal.mfj";
    StoreOptions options;
    options.journal_path = path;
    RecoveryReport report_in;
    Result<std::unique_ptr<Store>> store = Store::open(options, report_in);
    if (!store.ok()) {
      return 1;
    }
    (void)store.value()->begin_incarnation(ControllerId::from_u64(1), 1);
    (void)store.value()->commit_topology(bench::wide_fabric(16));
    (void)store.value()->commit_policy(bench::wide_policy(1));

    std::vector<EvidenceRecord> batch;
    batch.reserve(records);
    for (std::uint64_t i = 0; i < records; ++i) {
      EvidenceRecord evidence;
      evidence.key.kind = EvidenceKind::TargetHealth;
      evidence.key.subject = EvidenceSubject::of(bench::target_for(i, 16));
      evidence.key.source = SourceId::from_u64(1);
      evidence.revision = Revision::from_u64(i + 1);
      evidence.id = EvidenceId::from_u64(i + 1);
      evidence.observed_at = static_cast<Nanos>(i);
      evidence.ttl = 1000000;
      evidence.observed_by = ControllerIncarnation{ControllerId::from_u64(1), BootEpoch::from_u64(1), 1};
      evidence.payload.health = Health::Healthy;
      batch.push_back(evidence);
    }

    const WallClock::time_point commit_start = WallClock::now();
    std::uint64_t committed = 0;
    for (const EvidenceRecord& evidence : batch) {
      if (store.value()->observe_evidence(evidence).ok()) {
        ++committed;
      }
    }
    const double commit_seconds = seconds_since(commit_start);
    report("durable evidence commits", committed, commit_seconds);
    store.value().reset();

    const WallClock::time_point replay_start = WallClock::now();
    RecoveryReport recovery;
    Result<std::unique_ptr<Store>> reopened = Store::open(options, recovery);
    if (!reopened.ok()) {
      return 1;
    }
    const double replay_seconds = seconds_since(replay_start);
    report("records replayed from the journal", recovery.records_replayed, replay_seconds);
    std::printf("%-42s %llu records, %llu bytes, truncated=%s\n", "recovery summary",
                static_cast<unsigned long long>(recovery.records_replayed),
                static_cast<unsigned long long>(recovery.bytes_recovered),
                recovery.truncated ? "yes" : "no");
    const WallClock::time_point compact_start = WallClock::now();
    const Status compacted = reopened.value()->compact(static_cast<Nanos>(records));
    report("compaction passes", compacted.ok() ? 1u : 0u, seconds_since(compact_start));
  }

  // --- readiness evaluation --------------------------------------------------
  {
    const Topology topology = bench::wide_fabric(64);
    EvidenceStore evidence;
    const ControllerIncarnation who{ControllerId::from_u64(1), BootEpoch::from_u64(1), 1};
    const Policy policy = bench::wide_policy(1);
    for (const auto& [id, record] : topology.targets()) {
      (void)record;
      EvidenceRecord entry;
      entry.id = EvidenceId::from_u64(1);
      entry.key.kind = EvidenceKind::TargetHealth;
      entry.key.subject = EvidenceSubject::of(id);
      entry.key.source = SourceId::from_u64(1);
      entry.revision = Revision::from_u64(1);
      entry.id = EvidenceId::from_u64(1);
      entry.observed_at = 0;
      entry.ttl = 1000000000;
      entry.observed_by = who;
      entry.payload.health = Health::Healthy;
      (void)evidence.observe(entry);
    }
    const WallClock::time_point start = WallClock::now();
    std::uint64_t evaluations = 0;
    for (int i = 0; i < 200; ++i) {
      const ReadinessView view = ReadinessView::build(topology, evidence, policy, who, 1000);
      evaluations += view.size();
    }
    report("readiness evaluations", evaluations, seconds_since(start));
  }

  return 0;
}
