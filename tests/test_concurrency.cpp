// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency and shutdown. These tests join every thread they start and use no
// watchdogs: a hang would be a defect, not something to paper over.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"
#include "mf/transport/client.hpp"
#include "mf/transport/server.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

MF_TEST(concurrency, controller_is_safe_under_concurrent_mutation) {
  mftest::TempDir dir("conc-controller");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));

  constexpr int kThreads = 4;
  constexpr int kIterations = 40;
  std::atomic<int> proposals{0};
  std::atomic<int> ticks{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kIterations; ++i) {
        MaintenanceRequest request;
        request.title = "thread " + std::to_string(t) + " job " + std::to_string(i);
        request.reason = "concurrency";
        request.targets = {TargetId{TargetKind::Link,
                                    static_cast<std::uint32_t>((t % 4) * 2 + (i % 2) + 1)}};
        request.priority = static_cast<std::uint32_t>(i % 2);
        request.estimated_duration = 5 * kNanosPerMinute;
        request.requestor = requestor_from_name("concurrency");
        Result<JobId> created = controller->propose(request, "concurrency");
        if (created.ok()) {
          proposals.fetch_add(1);
          (void)controller->approve(created.value(), "concurrency");
        }
        if (controller->tick(clock.now()).ok()) {
          ticks.fetch_add(1);
        }
        clock.advance(kNanosPerMillisecond);
        // Concurrent readers must never observe a torn job.
        for (const JobSnapshot& snapshot : controller->list()) {
          MF_CHECK(snapshot.id.valid());
          MF_CHECK(snapshot.generation.valid());
        }
        (void)controller->conflicts();
        (void)controller->list();
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  MF_CHECK(proposals.load() > 0);
  MF_CHECK(ticks.load() > 0);
  MF_CHECK_EQ(controller->list().size(), static_cast<std::size_t>(proposals.load()));

  // Every admitted job must still be explainable, and no job may be in a
  // pre-removal state while reporting service removed.
  for (const JobSnapshot& snapshot : controller->list()) {
    MF_CHECK_OK(controller->explain(snapshot.id, "concurrency"));
    if (snapshot.service_removed) {
      MF_CHECK(snapshot.state == JobState::InMaintenance || snapshot.state == JobState::Verifying ||
               snapshot.state == JobState::Restoring || snapshot.state == JobState::Complete ||
               snapshot.state == JobState::Failed || snapshot.state == JobState::Blocked);
    }
  }
  MF_CHECK_OK(controller->shutdown());
}

MF_TEST(concurrency, drain_port_handles_parallel_leases) {
  auto drain = std::make_shared<LocalDrainPort>();
  constexpr int kThreads = 6;
  constexpr int kIterations = 30;
  std::atomic<int> granted{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      ControllerIncarnation requester;
      requester.controller = ControllerId::from_u64(1);
      requester.boot_epoch = BootEpoch::from_u64(1);
      requester.nonce = static_cast<std::uint64_t>(t + 1);
      for (int i = 0; i < kIterations; ++i) {
        DrainRequest request;
        request.job = JobId::from_u64(static_cast<std::uint64_t>(t * kIterations + i + 1));
        request.generation = GenerationId::from_u64(1);
        request.attempt = AttemptId{request.job, request.generation, AttemptOrdinal::from_u64(1)};
        request.targets = {TargetId{TargetKind::Link, static_cast<std::uint32_t>(t + 1)}};
        request.lease_duration = 10 * kNanosPerSecond;
        request.requester = requester;
        const DrainResponse response = drain->request_drain(request);
        if (!response.ok()) {
          refused.fetch_add(1);
          continue;
        }
        granted.fetch_add(1);
        MF_CHECK(drain->holds(response.lease.id));
        MF_CHECK(drain->release_drain(response.lease.id, requester).ok());
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  MF_CHECK_EQ(granted.load(), kThreads * kIterations);
  MF_CHECK_EQ(refused.load(), 0);
  MF_CHECK_EQ(drain->active_leases(), 0u);
}

namespace {

Status echo_handler(const Frame& request, Frame& response) {
  response.type = FrameType::Response;
  response.payload = request.payload;
  return ok_status();
}

}  // namespace

MF_TEST(concurrency, server_start_stop_cycles_return_to_a_clean_baseline) {
  for (int cycle = 0; cycle < 5; ++cycle) {
    TcpServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.workers = 2;
    TcpServer server(options, echo_handler);
    MF_CHECK_OK(server.start());
    MF_CHECK(server.running());
    const std::uint16_t port = server.port();
    MF_CHECK(port != 0);
    {
      Result<TcpClient> client = TcpClient::connect("127.0.0.1", port, 2 * kNanosPerSecond);
      MF_CHECK_OK(client);
      Frame request;
      request.type = FrameType::Request;
      request.request_id = 1;
      request.payload = {std::byte{7}, std::byte{8}};
      Result<Frame> response = client.value().call(request, 2 * kNanosPerSecond);
      MF_CHECK_OK(response);
      MF_CHECK_EQ(response.value().payload.size(), 2u);
      MF_CHECK_OK(client.value().close());
    }
    MF_CHECK_OK(server.stop());
    MF_CHECK(!server.running());
    // One request plus the client's goodbye frame.
    MF_CHECK_EQ(server.stats().handled, 2u);
    // A second stop is a no-op, not an error or a hang.
    MF_CHECK_OK(server.stop());
  }
}

MF_TEST(concurrency, concurrent_clients_are_all_served) {
  TcpServer::Options options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.workers = 4;
  TcpServer server(options, echo_handler);
  MF_CHECK_OK(server.start());
  const std::uint16_t port = server.port();

  constexpr int kClients = 8;
  constexpr int kRequests = 25;
  std::atomic<int> served{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([&, c]() {
      Result<TcpClient> client = TcpClient::connect("127.0.0.1", port, 5 * kNanosPerSecond);
      if (!client.ok()) {
        return;
      }
      for (int i = 0; i < kRequests; ++i) {
        Frame request;
        request.type = FrameType::Request;
        request.request_id = static_cast<std::uint64_t>(c * kRequests + i + 1);
        request.payload = {std::byte{static_cast<unsigned char>(c)}, std::byte{1}, std::byte{2}};
        Result<Frame> response = client.value().call(request, 5 * kNanosPerSecond);
        if (response.ok() && response.value().payload == request.payload) {
          served.fetch_add(1);
        }
      }
      (void)client.value().close();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  MF_CHECK_EQ(served.load(), kClients * kRequests);
  MF_CHECK_OK(server.stop());
  MF_CHECK_EQ(server.stats().handled,
              static_cast<std::uint64_t>(kClients * kRequests + kClients));
  MF_CHECK_EQ(server.stats().rejected, 0u);
}

MF_TEST(concurrency, server_stop_with_an_idle_client_does_not_hang) {
  TcpServer::Options options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.workers = 2;
  TcpServer server(options, echo_handler);
  MF_CHECK_OK(server.start());
  Result<TcpClient> client =
      TcpClient::connect("127.0.0.1", server.port(), 2 * kNanosPerSecond);
  MF_CHECK_OK(client);
  // The client is connected but idle; stopping must not wait for it.
  MF_CHECK_OK(server.stop());
  MF_CHECK(!server.running());
  // The client observes the disconnect rather than hanging forever.
  Frame request;
  request.type = FrameType::Request;
  request.request_id = 99;
  request.payload = {std::byte{1}};
  const Result<Frame> response = client.value().call(request, kNanosPerSecond);
  MF_CHECK(!response.ok());
}

MF_TEST_MAIN()
