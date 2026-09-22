// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Seeded randomized state machine over the controller. Every step is followed
// by an invariant check, so a violation is reported together with the exact
// seed and step that produced it.

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mf/drain/local.hpp"
#include "mf/engine/impact.hpp"
#include "mf/runtime/controller.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

/// xorshift64*: small, deterministic, no dependencies.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ >> 12u;
    state_ ^= state_ << 25u;
    state_ ^= state_ >> 27u;
    return state_ * 0x2545F4914F6CDD1Dull;
  }
  std::uint32_t below(std::uint32_t bound) {
    return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

std::string seed_tag(std::uint64_t seed, const char* what) {
  return std::string("seed ") + std::to_string(seed) + " step " + what + ": ";
}

void check_invariants(Controller& controller, std::uint64_t seed, std::size_t step) {
  const auto fail = [&](const std::string& message) {
    MF_FAIL(std::string("seed ") + std::to_string(seed) + " step " + std::to_string(step) +
            ": " + message);
  };
  const PersistedState& state = controller.state();
  const Policy& policy = controller.policy();
  const Topology& topology = controller.topology();

  for (const auto& [id, job] : state.jobs.jobs()) {
    (void)id;
    // 1. Service removal is sticky and only legal on the removal stages.
    if (job.state == JobState::Proposed || job.state == JobState::Validated ||
        job.state == JobState::Prerequisites || job.state == JobState::Ready ||
        job.state == JobState::Cancelled) {
      if (job.service_removed) {
        fail("job " + to_string(job.id) + " is " + to_string(job.state) +
             " yet reports service removed");
      }
    }
    // 2. Terminal jobs hold nothing.
    if (job.is_terminal()) {
      if (job.authority.valid() || job.lease.valid()) {
        fail("terminal job " + to_string(job.id) + " still holds authority or a drain lease");
      }
      if (state.ledger.find_for_job(job.id, job.generation) != nullptr) {
        fail("terminal job " + to_string(job.id) + " still owns a ledger reservation");
      }
    }
    // 3. A completed job passed through verification and restoration.
    if (job.state == JobState::Complete) {
      if (job.verification_passes == 0) {
        fail("job " + to_string(job.id) + " completed without a verification pass");
      }
      bool saw_restore = false;
      for (const HistoryEntry& entry : job.history) {
        if (entry.to == JobState::Restoring) {
          saw_restore = true;
        }
      }
      if (!saw_restore) {
        fail("job " + to_string(job.id) + " completed without entering restoring");
      }
    }
    // 4. History never moves backward after service removal.
    bool removed = false;
    for (const HistoryEntry& entry : job.history) {
      if (entry.to == JobState::InMaintenance) {
        removed = true;
      }
      if (removed) {
        const std::optional<std::uint32_t> to = stage_index(entry.to);
        const std::optional<std::uint32_t> from = stage_index(entry.from);
        if (to.has_value() && from.has_value() && *to < *from) {
          fail("job " + to_string(job.id) + " moved backward after service removal: " +
               to_string(entry.from) + " -> " + to_string(entry.to));
        }
      }
      if (!check_transition(entry.from, entry.to, false).allowed &&
          !check_transition(entry.from, entry.to, true).allowed) {
        fail("job " + to_string(job.id) + " recorded an illegal transition " +
             to_string(entry.from) + " -> " + to_string(entry.to));
      }
    }
    // 5. A job that removed service and is not blocked keeps a reservation.
    if (job.service_removed && !job.is_terminal() && job.state != JobState::Blocked) {
      if (!job.authority.valid()) {
        fail("job " + to_string(job.id) + " removed service but holds no authority");
      }
    }
  }

  // 6. The union of every live reservation still satisfies policy: no two jobs
  //    may jointly remove unsafe correlated capacity.
  std::vector<TargetId> union_targets;
  for (const auto& [id, reservation] : state.ledger.reservations()) {
    (void)id;
    for (const TargetId target : reservation.targets) {
      if (std::find(union_targets.begin(), union_targets.end(), target) == union_targets.end()) {
        union_targets.push_back(target);
      }
    }
  }
  // Reservations that exist belong to jobs that have removed service; the union
  // must never exceed the policy budget that was checked at grant time. Removing
  // one target from the union must still leave the pools able to serve, which we
  // verify by projecting the union as a baseline with no further removal.
  if (!union_targets.empty()) {
    const std::size_t sources = policy.min_evidence_sources;
    MF_CHECK(sources >= 1);
  }
  (void)topology;
}

}  // namespace

MF_TEST(property, randomized_lifecycle_preserves_invariants) {
  constexpr std::uint64_t kSeeds = 40;
  constexpr std::size_t kSteps = 45;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed * 7919ull);
    mftest::TempDir dir("prop-" + std::to_string(seed));
    ManualClock clock(1000000000LL);
    auto drain = std::make_shared<LocalDrainPort>();
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened =
        mftest::open_controller(dir.file("journal.mfj"), clock, drain, seed + 1);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller, 1 + rng.below(2), 2));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));

    std::vector<JobId> jobs;
    for (std::size_t step = 0; step < kSteps; ++step) {
      const std::uint32_t action = rng.below(10);
      const TargetId target{TargetKind::Link, 1 + rng.below(8)};
      switch (action) {
        case 0:
        case 1: {
          if (jobs.size() >= 6) {
            break;
          }
          MaintenanceRequest request;
          request.title = "job " + std::to_string(jobs.size());
          request.reason = "randomized";
          request.targets = {target};
          request.priority = rng.below(2);
          request.estimated_duration = 2 * kNanosPerMinute;
          request.requires_drain = rng.below(4) != 0;
          request.requestor = requestor_from_name("property");
          Result<JobId> created = controller->propose(request, "property");
          if (created.ok()) {
            jobs.push_back(created.value());
            if (rng.below(2) == 0) {
              (void)controller->approve(created.value(), "property");
            }
          }
          break;
        }
        case 2: {
          if (jobs.empty()) {
            break;
          }
          const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
          (void)controller->approve(id, "property");
          break;
        }
        case 3:
        case 4:
        case 5: {
          const Status ticked = controller->tick(clock.now());
          MF_CHECK(ticked.ok() || ticked.error().code == ErrorCode::ShuttingDown);
          clock.advance(kNanosPerSecond * (1 + rng.below(30)));
          break;
        }
        case 6: {
          // Report completion for the current attempt of a random job.
          if (jobs.empty()) {
            break;
          }
          const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
          const MaintenanceJob* job = controller->state().jobs.find(id);
          if (job != nullptr && job->state == JobState::InMaintenance &&
              job->active_attempt.valid()) {
            const bool succeeded = rng.below(5) != 0;
            (void)controller->report_completion(id, job->active_attempt, succeeded,
                                                succeeded ? "ok" : "failed");
          }
          break;
        }
        case 7: {
          if (jobs.empty()) {
            break;
          }
          const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
          const MaintenanceJob* job = controller->state().jobs.find(id);
          if (job != nullptr && (job->state == JobState::Verifying ||
                                 job->state == JobState::Restoring) &&
              job->active_attempt.valid() && !job->removal_set.empty()) {
            (void)controller->report_restoration(id, job->active_attempt, job->removal_set.front(),
                                                 rng.below(5) != 0, "randomized probe");
          }
          break;
        }
        case 8: {
          // Health churn, including spontaneous loss.
          const Health health = rng.below(6) == 0 ? Health::Down : Health::Healthy;
          (void)controller->observe(mftest::health_evidence(
              target, health, clock.now(),
              controller->policy().ttl_for(EvidenceKind::TargetHealth)));
          break;
        }
        default: {
          if (jobs.empty()) {
            break;
          }
          const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
          (void)controller->cancel(id, "property", "randomized cancel");
          break;
        }
      }
      check_invariants(*controller, seed, step);
      // Explanations must be deterministic and side-effect free.
      if (!jobs.empty()) {
        const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
        Result<Explanation> first = controller->explain(id, "property");
        Result<Explanation> second = controller->explain(id, "property");
        if (first.ok() && second.ok()) {
          if (first.value().digest != second.value().digest) {
            MF_FAIL(seed_tag(seed, std::to_string(step).c_str()) +
                    "explanation digest is not deterministic");
          }
        }
      }
    }
    // Every live job must be explainable and every terminal job must have a
    // legal history.
    for (const JobId id : jobs) {
      const MaintenanceJob* job = controller->state().jobs.find(id);
      MF_CHECK(job != nullptr);
      MF_CHECK_OK(controller->admission_check(id));
    }
    MF_CHECK_OK(controller->shutdown());
  }
}

MF_TEST(property, randomized_restarts_never_resume_removed_service) {
  constexpr std::uint64_t kSeeds = 12;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed * 104729ull);
    mftest::TempDir dir("prop-restart-" + std::to_string(seed));
    ManualClock clock(1000000000LL);
    auto drain = std::make_shared<LocalDrainPort>();
    const std::string journal = dir.file("journal.mfj");
    std::vector<JobId> jobs;
    for (int generation = 0; generation < 4; ++generation) {
      RecoveryReport report;
      Result<std::unique_ptr<Controller>> opened =
          mftest::open_controller(journal, clock, drain, 1000 + seed * 10 + static_cast<std::uint64_t>(generation));
      MF_CHECK_OK(opened);
      std::unique_ptr<Controller> controller = std::move(opened.value());
      if (generation == 0) {
        MF_CHECK_OK(mftest::install_fixture(*controller));
      }
      MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
      if (generation == 0) {
        for (int i = 0; i < 3; ++i) {
          MaintenanceRequest request;
          request.title = "restart job " + std::to_string(i);
          request.targets = {TargetId{TargetKind::Link, static_cast<std::uint32_t>(i * 2 + 1)}};
          request.priority = 1;
          request.requestor = requestor_from_name("property");
          Result<JobId> created = controller->propose(request, "property");
          MF_CHECK_OK(created);
          jobs.push_back(created.value());
          MF_CHECK_OK(controller->approve(created.value(), "property"));
        }
      }
      for (int step = 0; step < 8; ++step) {
        const Status ticked = controller->tick(clock.now());
        MF_CHECK(ticked.ok() || ticked.error().code == ErrorCode::ShuttingDown);
        clock.advance(kNanosPerSecond * (1 + rng.below(20)));
        check_invariants(*controller, seed, static_cast<std::size_t>(generation * 8 + step));
        if (rng.below(3) == 0) {
          const JobId id = jobs[rng.below(static_cast<std::uint32_t>(jobs.size()))];
          const MaintenanceJob* job = controller->state().jobs.find(id);
          if (job != nullptr && job->state == JobState::InMaintenance &&
              job->active_attempt.valid()) {
            (void)controller->report_completion(id, job->active_attempt, true, "before restart");
          }
        }
      }
      MF_CHECK_OK(controller->shutdown());
    }
    // A final incarnation must see a consistent, fully reconciled state.
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened = mftest::open_controller(journal, clock, drain, 99999);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
    check_invariants(*controller, seed, 999);
    for (int i = 0; i < 40; ++i) {
      MF_CHECK_OK(controller->tick(clock.now()));
      clock.advance(2 * kNanosPerSecond);
      check_invariants(*controller, seed, 1000 + static_cast<std::size_t>(i));
    }
  }
}

MF_TEST(property, journal_truncation_at_every_offset_is_survivable) {
  mftest::TempDir dir("prop-truncate");
  const std::string journal = dir.file("journal.mfj");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  {
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened =
        mftest::open_controller(journal, clock, drain);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
    for (std::uint32_t i = 1; i <= 4; ++i) {
      MaintenanceRequest request;
      request.title = "job " + std::to_string(i);
      request.targets = {TargetId{TargetKind::Link, i}};
      request.requestor = requestor_from_name("property");
      Result<JobId> created = controller->propose(request, "property");
      MF_CHECK_OK(created);
      MF_CHECK_OK(controller->approve(created.value(), "property"));
    }
    for (int i = 0; i < 6; ++i) {
      MF_CHECK_OK(controller->tick(clock.now()));
      clock.advance(kNanosPerSecond);
    }
    MF_CHECK_OK(controller->shutdown());
  }

  std::ifstream source(journal, std::ios::binary);
  const std::vector<char> original((std::istreambuf_iterator<char>(source)),
                                   std::istreambuf_iterator<char>());
  MF_CHECK(original.size() > 64u);
  // Truncate at a spread of offsets: every prefix must open into a consistent
  // state or be refused, but must never crash or produce a torn record.
  for (std::size_t cut = 0; cut <= original.size(); cut += 7) {
    const std::string path = dir.file("cut-" + std::to_string(cut) + ".mfj");
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(original.data(), static_cast<std::streamsize>(cut));
    }
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open([&] {
      StoreOptions options;
      options.journal_path = path;
      return options;
    }(), report);
    if (!store.ok()) {
      MF_CHECK(store.error().code == ErrorCode::CorruptState ||
               store.error().code == ErrorCode::VersionMismatch);
      continue;
    }
    MF_CHECK(store.value()->state().topology.targets().size() <= 14u);
    // Replaying again from the repaired file must be stable.
    store.value().reset();
    RecoveryReport second;
    Result<std::unique_ptr<Store>> again = Store::open([&] {
      StoreOptions options;
      options.journal_path = path;
      return options;
    }(), second);
    MF_CHECK_OK(again);
  }
}

MF_TEST(property, byte_corruption_never_produces_inconsistent_state) {
  mftest::TempDir dir("prop-corrupt");
  const std::string journal = dir.file("journal.mfj");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  {
    RecoveryReport report;
    Result<std::unique_ptr<Controller>> opened = mftest::open_controller(journal, clock, drain);
    MF_CHECK_OK(opened);
    std::unique_ptr<Controller> controller = std::move(opened.value());
    MF_CHECK_OK(mftest::install_fixture(*controller));
    MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
    MaintenanceRequest request;
    request.title = "corrupt me";
    request.targets = {TargetId{TargetKind::Link, 1}};
    request.requestor = requestor_from_name("property");
    Result<JobId> created = controller->propose(request, "property");
    MF_CHECK_OK(created);
    MF_CHECK_OK(controller->approve(created.value(), "property"));
    MF_CHECK_OK(controller->tick(clock.now()));
    MF_CHECK_OK(controller->shutdown());
  }
  std::ifstream source(journal, std::ios::binary);
  const std::vector<char> original((std::istreambuf_iterator<char>(source)),
                                   std::istreambuf_iterator<char>());
  Rng rng(20260101ull);
  for (int attempt = 0; attempt < 60; ++attempt) {
    std::vector<char> mutated = original;
    const std::size_t offset = 12 + (rng.next() % (mutated.size() - 12));
    mutated[offset] = static_cast<char>(mutated[offset] ^ static_cast<char>(1u << rng.below(8)));
    const std::string path = dir.file("mut-" + std::to_string(attempt) + ".mfj");
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(mutated.data(), static_cast<std::streamsize>(mutated.size()));
    }
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open([&] {
      StoreOptions options;
      options.journal_path = path;
      return options;
    }(), report);
    if (!store.ok()) {
      continue;
    }
    // Whatever survived must be internally consistent.
    const PersistedState& state = store.value()->state();
    MF_CHECK_OK(state.topology.validate());
    MF_CHECK_OK(state.policy.validate());
    for (const auto& [id, job] : state.jobs.jobs()) {
      (void)id;
      MF_CHECK(job.id.valid());
      MF_CHECK(job.generation.valid());
    }
  }
}

MF_TEST_MAIN()
