// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Distributed behaviour proven with independent OS processes and real framed TCP
// sockets: a reference drain service and the daemon are started as separate
// processes, driven over the wire, killed, restarted and fenced.

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "apps/common/api.hpp"
#include "mf/config/text.hpp"
#include "mf/drain/remote.hpp"
#include "mf/runtime/controller.hpp"
#include "mf/transport/protocol.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

#ifndef MF_TEST_BINDIR
#define MF_TEST_BINDIR "."
#endif

std::string binary(const std::string& name) {
#ifdef _WIN32
  return std::string(MF_TEST_BINDIR) + "/" + name + ".exe";
#else
  return std::string(MF_TEST_BINDIR) + "/" + name;
#endif
}

/// Parses "listening on 127.0.0.1:PORT" from a child's stdout.
std::optional<std::uint16_t> wait_for_port(mftest::ChildProcess& child) {
  for (int attempt = 0; attempt < 60; ++attempt) {
    const std::optional<std::string> line = child.read_line(30LL * kNanosPerSecond);
    if (!line.has_value()) {
      return std::nullopt;
    }
    const std::size_t colon = line->rfind(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::optional<std::uint32_t> port = parse_u32(line->substr(colon + 1));
    if (port.has_value() && *port > 0 && *port <= 65535u) {
      return static_cast<std::uint16_t>(*port);
    }
  }
  return std::nullopt;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

}  // namespace

MF_TEST(transport, remote_drain_port_talks_to_an_independent_process) {
  mftest::TempDir dir("tx-drain");
  std::optional<mftest::ChildProcess> child =
      mftest::ChildProcess::spawn(binary("mf-drain-sim"),
                                  {"--bind", "127.0.0.1", "--port", "0", "--log-level", "warn"},
                                  true);
  MF_CHECK(child.has_value());
  const std::optional<std::uint16_t> port = wait_for_port(*child);
  MF_CHECK(port.has_value());
  MF_CHECK(child->running());

  RemoteDrainOptions options;
  options.host = "127.0.0.1";
  options.port = *port;
  options.deadline = 5 * kNanosPerSecond;
  RemoteDrainPort drain(options);

  ControllerIncarnation requester;
  requester.controller = ControllerId::from_u64(1);
  requester.boot_epoch = BootEpoch::from_u64(1);
  requester.nonce = 77;

  DrainRequest request;
  request.job = JobId::from_u64(1);
  request.generation = GenerationId::from_u64(1);
  request.attempt = AttemptId{request.job, request.generation, AttemptOrdinal::from_u64(1)};
  request.targets = {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 2}};
  request.lease_duration = 10 * kNanosPerSecond;
  request.reason = "integration";
  request.requester = requester;

  DrainResponse response = drain.request_drain(request);
  MF_CHECK_OK(response.status);
  MF_CHECK(response.lease.id.valid());
  MF_CHECK_EQ(response.lease.targets.size(), 2u);
  MF_CHECK_EQ(response.lease.state, DrainState::Drained);
  MF_CHECK_EQ(response.lease.attempt, request.attempt);
  MF_CHECK_EQ(response.lease.owner, requester);

  // The lease survives a client reconnect because it lives in the service.
  drain.disconnect();
  DrainResponse queried = drain.query_drain(response.lease.id, requester);
  MF_CHECK_OK(queried.status);
  MF_CHECK_EQ(queried.lease.id, response.lease.id);
  MF_CHECK(queried.lease.epoch >= response.lease.epoch);

  // A refresh from a different incarnation is fenced by the service.
  ControllerIncarnation intruder = requester;
  intruder.nonce = 12345;
  const DrainResponse fenced = drain.refresh_drain(response.lease.id, kNanosPerSecond, intruder);
  MF_CHECK(!fenced.status.ok());
  MF_CHECK(fenced.status.error().code == ErrorCode::StaleIncarnation ||
           fenced.status.error().code == ErrorCode::DrainFailed);

  MF_CHECK_OK(drain.release_drain(response.lease.id, requester));
  MF_CHECK(!drain.query_drain(response.lease.id, requester).status.ok());

  // A hard kill makes the endpoint unreachable; the port reports a failure
  // rather than silently succeeding.
  child->terminate();
  const int code = child->wait();
  (void)code;
  drain.disconnect();
  const DrainResponse dead = drain.request_drain(request);
  MF_CHECK(!dead.status.ok());
  MF_CHECK(dead.status.error().code == ErrorCode::DrainFailed);
  MF_CHECK(drain.failures() > 0);
}

MF_TEST(transport, drain_service_injected_failure_is_visible_over_the_wire) {
  mftest::TempDir dir("tx-drain-fail");
  std::optional<mftest::ChildProcess> child = mftest::ChildProcess::spawn(
      binary("mf-drain-sim"),
      {"--bind", "127.0.0.1", "--port", "0", "--fail-next", "1", "--log-level", "warn"}, true);
  MF_CHECK(child.has_value());
  const std::optional<std::uint16_t> port = wait_for_port(*child);
  MF_CHECK(port.has_value());

  RemoteDrainOptions options;
  options.host = "127.0.0.1";
  options.port = *port;
  options.deadline = 5 * kNanosPerSecond;
  RemoteDrainPort drain(options);

  ControllerIncarnation requester;
  requester.controller = ControllerId::from_u64(1);
  requester.boot_epoch = BootEpoch::from_u64(1);
  requester.nonce = 5;
  DrainRequest request;
  request.job = JobId::from_u64(1);
  request.generation = GenerationId::from_u64(1);
  request.attempt = AttemptId{request.job, request.generation, AttemptOrdinal::from_u64(1)};
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.lease_duration = 10 * kNanosPerSecond;
  request.requester = requester;

  const DrainResponse failed = drain.request_drain(request);
  MF_CHECK(!failed.status.ok());
  const DrainResponse second = drain.request_drain(request);
  MF_CHECK_OK(second.status);
  (void)child->kill_and_wait();
}

MF_TEST(transport, daemon_is_driven_over_real_sockets_and_fenced_across_a_kill) {
  mftest::TempDir dir("tx-daemon");
  const std::string topology_path = dir.file("topology.mftopo");
  const std::string policy_path = dir.file("policy.mfpolicy");
  write_text(topology_path, render_topology(mftest::make_topology()));
  write_text(policy_path, render_policy(mftest::make_policy(1)));

  // The drain service runs as its own process.
  std::optional<mftest::ChildProcess> drain_process = mftest::ChildProcess::spawn(
      binary("mf-drain-sim"), {"--bind", "127.0.0.1", "--port", "0", "--log-level", "warn"}, true);
  MF_CHECK(drain_process.has_value());
  const std::optional<std::uint16_t> drain_port = wait_for_port(*drain_process);
  MF_CHECK(drain_port.has_value());

  const std::string store = dir.file("state/journal.mfj");
  const std::vector<std::string> daemon_args = {
      "--store",       store,
      "--topology",    topology_path,
      "--policy",      policy_path,
      "--bind",        "127.0.0.1",
      "--port",        "0",
      "--workers",     "3",
      "--tick-interval", "100ms",
      "--health-provider", "static-healthy",
      "--drain-address", std::string("127.0.0.1:") + std::to_string(*drain_port),
      "--log-level",   "warn"};

  std::optional<mftest::ChildProcess> daemon =
      mftest::ChildProcess::spawn(binary("mfd"), daemon_args, true);
  MF_CHECK(daemon.has_value());
  const std::optional<std::uint16_t> port = wait_for_port(*daemon);
  MF_CHECK(port.has_value());
  MF_CHECK(daemon->running());

  JobId job;
  AttemptId attempt;
  {
    Result<std::unique_ptr<app::RemoteApi>> api =
        app::RemoteApi::connect("127.0.0.1", *port, 10 * kNanosPerSecond);
    MF_CHECK_OK(api);
    MaintenanceRequest request;
    request.title = "remote maintenance";
    request.reason = "integration";
    request.targets = {TargetId{TargetKind::Link, 1}};
    request.priority = 1;
    request.requestor = requestor_from_name("integration");
    Result<JobId> created = api.value()->propose(request, "integration");
    MF_CHECK_OK(created);
    job = created.value();
    MF_CHECK_OK(api.value()->approve(job, "integration"));

    // Wait for the daemon's own tick thread to carry the job into maintenance.
    // The daemon is free to block the job transiently (for example while the
    // health provider has not published yet) and retry, so the test waits for
    // the start rather than for the first non-planning state.
    JobState state = JobState::Validated;
    for (int i = 0; i < 300; ++i) {
      Result<JobSnapshot> snapshot = api.value()->status(job);
      if (snapshot.ok()) {
        state = snapshot.value().state;
        if (state == JobState::InMaintenance || is_terminal(state)) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (state != JobState::InMaintenance) {
      Result<JobSnapshot> diagnostic = api.value()->status(job);
      Result<std::string> explanation = api.value()->explain(job, "diagnostic");
      MF_FAIL("job reached " + std::string(to_string(state)) +
              (diagnostic.ok()
                   ? " (" + std::string(to_string(diagnostic.value().block)) + ": " +
                         diagnostic.value().block_detail + ")"
                   : std::string()) +
              (explanation.ok() ? "\n" + explanation.value() : std::string()));
    }
    Result<JobSnapshot> snapshot = api.value()->status(job);
    MF_CHECK_OK(snapshot);
    MF_CHECK(snapshot.value().service_removed);
    MF_CHECK(snapshot.value().authority.valid());
    attempt = snapshot.value().active_attempt;
    MF_CHECK(attempt.valid());

    Result<std::string> explanation = api.value()->explain(job, "remote");
    MF_CHECK_OK(explanation);
    MF_CHECK(explanation.value().find("explanation digest=") != std::string::npos);

    // Errors cross the wire with their real code: a well-formed but stale
    // attempt is fenced, and a structurally invalid one is refused earlier.
    AttemptId stale = attempt;
    stale.ordinal = AttemptOrdinal::from_u64(attempt.ordinal.value() + 7);
    const Status late = api.value()->report_completion(job, stale, true, "stale attempt");
    MF_CHECK(!late.ok());
    MF_CHECK(late.error().code == ErrorCode::StaleAttempt ||
             late.error().code == ErrorCode::StaleGeneration);
    const Status malformed = api.value()->report_completion(job, AttemptId{}, true, "no attempt");
    MF_CHECK(!malformed.ok());
    (void)api.value()->tick(1);
  }

  // Kill the daemon hard while the scope is out of service.
  const int killed = daemon->kill_and_wait();
  (void)killed;

  // Restart it against the same journal.
  std::optional<mftest::ChildProcess> restarted =
      mftest::ChildProcess::spawn(binary("mfd"), daemon_args, true);
  MF_CHECK(restarted.has_value());
  const std::optional<std::uint16_t> second_port = wait_for_port(*restarted);
  MF_CHECK(second_port.has_value());
  {
    Result<std::unique_ptr<app::RemoteApi>> api =
        app::RemoteApi::connect("127.0.0.1", *second_port, 10 * kNanosPerSecond);
    MF_CHECK_OK(api);
    Result<JobSnapshot> snapshot = api.value()->status(job);
    MF_CHECK_OK(snapshot);
    MF_CHECK(snapshot.value().service_removed);
    MF_CHECK(snapshot.value().state == JobState::Blocked || snapshot.value().state == JobState::Verifying);
    MF_CHECK(!snapshot.value().active_attempt.valid() ||
             snapshot.value().active_attempt != attempt);

    // The attempt from the previous process incarnation cannot complete the job.
    const Status fenced = api.value()->report_completion(job, attempt, true, "old process");
    MF_CHECK(!fenced.ok());
    MF_CHECK(fenced.error().code == ErrorCode::StaleAttempt ||
             fenced.error().code == ErrorCode::StaleGeneration);

    // The scope is still out of service: capacity stays reserved in the new
    // incarnation rather than being silently released.
    MF_CHECK(snapshot.value().authority.valid());

    Result<std::string> conflicts = api.value()->conflicts();
    MF_CHECK_OK(conflicts);
    MF_CHECK(conflicts.value().find("live reservations") != std::string::npos);
  }
  (void)restarted->kill_and_wait();
  drain_process->terminate();
  (void)drain_process->wait();
}

MF_TEST(transport, daemon_refuses_a_second_scope_on_the_same_capacity) {
  mftest::TempDir dir("tx-conflict");
  const std::string topology_path = dir.file("topology.mftopo");
  const std::string policy_path = dir.file("policy.mfpolicy");
  write_text(topology_path, render_topology(mftest::make_topology()));
  write_text(policy_path, render_policy(mftest::make_policy(1)));

  const std::vector<std::string> args = {"--store", dir.file("state/journal.mfj"),
                                         "--topology", topology_path,
                                         "--policy", policy_path,
                                         "--bind", "127.0.0.1",
                                         "--port", "0",
                                         "--tick-interval", "50ms",
                                         "--health-provider", "static-healthy",
                                         "--log-level", "warn"};
  std::optional<mftest::ChildProcess> daemon = mftest::ChildProcess::spawn(binary("mfd"), args, true);
  MF_CHECK(daemon.has_value());
  const std::optional<std::uint16_t> port = wait_for_port(*daemon);
  MF_CHECK(port.has_value());

  Result<std::unique_ptr<app::RemoteApi>> api =
      app::RemoteApi::connect("127.0.0.1", *port, 10 * kNanosPerSecond);
  MF_CHECK_OK(api);

  const auto propose = [&](const std::string& title, TargetId target) {
    MaintenanceRequest request;
    request.title = title;
    request.targets = {target};
    request.requestor = requestor_from_name("integration");
    Result<JobId> created = api.value()->propose(request, "integration");
    MF_CHECK_OK(created);
    MF_CHECK_OK(api.value()->approve(created.value(), "integration"));
    return created.value();
  };
  const JobId first = propose("first", TargetId{TargetKind::Link, 1});
  const JobId second = propose("second", TargetId{TargetKind::Link, 2});

  bool first_removed = false;
  bool second_stayed = false;
  for (int i = 0; i < 200; ++i) {
    Result<JobSnapshot> a = api.value()->status(first);
    Result<JobSnapshot> b = api.value()->status(second);
    if (a.ok() && b.ok()) {
      first_removed = a.value().service_removed || is_terminal(a.value().state);
      second_stayed = !b.value().service_removed;
      if (first_removed && second_stayed && b.value().state == JobState::Blocked) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  MF_CHECK(first_removed);
  MF_CHECK(second_stayed);
  Result<JobSnapshot> blocked = api.value()->status(second);
  MF_CHECK_OK(blocked);
  MF_CHECK_EQ(blocked.value().state, JobState::Blocked);
  // The two links share a correlated rack domain as well as capacity, so any of
  // these refusals is correct.
  MF_CHECK(blocked.value().block == BlockReason::RedundancyViolation ||
           blocked.value().block == BlockReason::ConflictingMaintenance ||
           blocked.value().block == BlockReason::FailureDomainLimit);

  Result<std::string> report = api.value()->arbitration();
  MF_CHECK_OK(report);
  daemon->terminate();
  (void)daemon->wait();
}

MF_TEST_MAIN()
