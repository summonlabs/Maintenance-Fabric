// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Adversarial input: malformed configuration, truncated and corrupted records,
// hostile frame payloads, duplicate identities, oversized declarations and
// reordered events. Nothing here may crash, hang, over-allocate or be accepted.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "mf/config/text.hpp"
#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"
#include "mf/transport/client.hpp"
#include "mf/transport/frame.hpp"
#include "mf/transport/protocol.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x2545F4914F6CDD1Dull : seed) {}
  std::uint64_t next() {
    state_ ^= state_ >> 12u;
    state_ ^= state_ << 25u;
    state_ ^= state_ >> 27u;
    return state_ * 0x9E3779B97F4A7C15ull;
  }
  std::uint8_t byte() { return static_cast<std::uint8_t>(next() & 0xFFull); }

 private:
  std::uint64_t state_;
};

}  // namespace

MF_TEST(adversarial, configuration_parser_rejects_hostile_input) {
  ParseStats stats;
  const std::vector<std::string> hostile = {
      "",
      "\n\n\n",
      "target",
      "target kind",
      "target kind=link",
      "target kind=link id=1",
      "target kind=link id=1 domains=",
      "target kind=link id=1 domains=,,",
      "target kind=link id=1 domains=rack:1 parent=link:1 extra=1",
      "target kind=link id=99999999999999999999 domains=rack:1",
      "domain kind=rack id=0 name=x",
      "domain kind=rack id=1 name=" + std::string(limits::kMaxNameLength + 1, 'x'),
      "pool id=1 members=link:1",
      "redundancy pool=1 min_viable=1 tolerated_losses=4294967295",
      "window id=1 opens=epoch:1 closes=epoch:2 min_lead=-5s",
      "window id=1 opens=epoch:1 closes=epoch:2 on_close=explode",
      "tier index=0",
      "evidence_ttl kind=nonsense ttl=5s",
      "setting key= value=1",
      "setting key=min_evidence_sources value=-1",
      "\x00\x01\x02",
  };
  for (const std::string& text : hostile) {
    const Result<Topology> topology = load_topology_text(text, stats);
    const Result<Policy> policy = load_policy_text(text, 0, stats);
    const bool topology_ok = topology.ok();
    const bool policy_ok = policy.ok();
    // The empty document is valid for both; everything else must be refused by
    // at least one of the two parsers and never accepted by the wrong one.
    if (!text.empty() && text != "\n\n\n") {
      MF_CHECK(!topology_ok || !policy_ok || text == "\n\n\n");
    }
    if (topology_ok) {
      MF_CHECK_OK(topology.value().validate());
    }
    if (policy_ok) {
      MF_CHECK_OK(policy.value().validate());
    }
  }
}

MF_TEST(adversarial, randomized_configuration_fuzzing) {
  Rng rng(0xC0FFEEull);
  ParseStats stats;
  const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789=:. _-,\n";
  for (int attempt = 0; attempt < 400; ++attempt) {
    std::string text;
    const std::size_t length = 1 + (rng.next() % 80);
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(alphabet[rng.next() % (sizeof(alphabet) - 1)]);
    }
    const Result<Topology> topology = load_topology_text(text, stats);
    if (topology.ok()) {
      MF_CHECK_OK(topology.value().validate());
      const Result<Topology> again = load_topology_text(render_topology(topology.value()), stats);
      MF_CHECK_OK(again);
      MF_CHECK_EQ(again.value().digest(), topology.value().digest());
    }
    const Result<Policy> policy = load_policy_text(text, 0, stats);
    if (policy.ok()) {
      MF_CHECK_OK(policy.value().validate());
    }
  }
}

MF_TEST(adversarial, frame_decoder_rejects_hostile_bytes) {
  Rng rng(0xF00Dull);
  for (int attempt = 0; attempt < 500; ++attempt) {
    std::vector<std::byte> bytes(kFrameHeaderBytes + (rng.next() % 64));
    for (std::byte& value : bytes) {
      value = static_cast<std::byte>(rng.byte());
    }
    const Result<Frame> frame = decode_frame(bytes);
    MF_CHECK(!frame.ok());
  }
  // A valid frame with a corrupted payload checksum.
  Frame frame;
  frame.type = FrameType::Request;
  frame.request_id = 9;
  frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  std::vector<std::byte> encoded = encode_frame(frame);
  encoded.back() = static_cast<std::byte>(static_cast<unsigned char>(encoded.back()) ^ 0xFFu);
  MF_CHECK(!decode_frame(encoded).ok());
  // Truncated header and truncated payload.
  MF_CHECK(!decode_frame(std::span<const std::byte>(encoded.data(), 8)).ok());
  MF_CHECK(!decode_frame(std::span<const std::byte>(encoded.data(), encoded.size() - 1)).ok());
  // A declared length beyond the bound is refused before allocating.
  std::vector<std::byte> huge = encode_frame(frame);
  huge[20] = std::byte{0x7F};
  huge[21] = std::byte{0xFF};
  huge[22] = std::byte{0xFF};
  huge[23] = std::byte{0xFF};
  const Result<FrameHeader> header = decode_frame_header(huge);
  MF_CHECK(!header.ok());
}

MF_TEST(adversarial, rpc_body_decoders_reject_hostile_bytes) {
  Rng rng(0xBEEFull);
  for (int attempt = 0; attempt < 400; ++attempt) {
    std::vector<std::byte> bytes(rng.next() % 96);
    for (std::byte& value : bytes) {
      value = static_cast<std::byte>(rng.byte());
    }
    const std::span<const std::byte> view(bytes);
    MF_CHECK(!decode_envelope(view).ok());
    MF_CHECK(!decode_request_body(view).ok());
    MF_CHECK(!decode_snapshot(view).ok());
    MF_CHECK(!decode_snapshot_list(view).ok());
    MF_CHECK(!decode_completion_report(view).ok());
    MF_CHECK(!decode_restoration_report(view).ok());
    MF_CHECK(!decode_evidence_body(view).ok());
    MF_CHECK(!decode_drain_request_body(view).ok());
    MF_CHECK(!decode_drain_lease_body(view).ok());
    MF_CHECK(!decode_drain_lease_ref(view).ok());
    MF_CHECK(!decode_job_actor(view).ok());
    MF_CHECK(!decode_quarantine_clear(view).ok());
    MF_CHECK(!decode_job_ref(view).ok());
    MF_CHECK(!decode_actor(view).ok());
  }
  // Bounded counts are honoured even with a valid layout prefix.
  ByteWriter writer;
  writer.u16(1);
  writer.u32(0xFFFFFFFFu);
  MF_CHECK(!decode_snapshot_list(writer.view()).ok());
  MF_CHECK(!decode_drain_lease_body(writer.view()).ok());
  MF_CHECK(!decode_completion_report(writer.view()).ok());
}

MF_TEST(adversarial, record_decoders_reject_hostile_bytes) {
  Rng rng(0x1234ull);
  Topology topology;
  Policy policy;
  MaintenanceJob job;
  EvidenceRecord evidence;
  AuthorityReservation reservation;
  DrainLease lease;
  QuarantineRecord quarantine;
  for (int attempt = 0; attempt < 400; ++attempt) {
    Record record;
    record.type = static_cast<RecordType>(
        1 + (rng.next() % static_cast<std::uint64_t>(RecordType::Marker)));
    const std::size_t length = rng.next() % 128;
    record.payload.resize(length);
    for (std::byte& value : record.payload) {
      value = static_cast<std::byte>(rng.byte());
    }
    (void)decode_topology_record(record, topology);
    (void)decode_policy_record(record, policy);
    (void)decode_job_record(record, job);
    (void)decode_evidence_record(record, evidence);
    (void)decode_reservation_record(record, reservation);
    (void)decode_lease_record(record, lease);
    (void)decode_quarantine_record(record, quarantine);
    (void)decode_job_erased_record(record, job.id);
    std::vector<Record> inner;
    (void)decode_transaction_record(record, inner);
    for (const Record& sub : inner) {
      MF_CHECK(validate_record(sub).ok());
    }
  }
}

MF_TEST(adversarial, transaction_records_cannot_nest_or_exceed_bounds) {
  Record nested;
  nested.type = RecordType::Transaction;
  nested.payload = {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                    std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  const Record outer = encode_transaction_record({encode_marker_record(1)});
  std::vector<Record> decoded;
  MF_CHECK_OK(decode_transaction_record(outer, decoded));
  MF_CHECK_EQ(decoded.size(), 1u);
  MF_CHECK(!decode_transaction_record(nested, decoded).ok());

  Record empty_transaction;
  empty_transaction.type = RecordType::Transaction;
  ByteWriter writer;
  writer.u16(1);
  writer.u32(0);
  empty_transaction.payload = writer.take();
  MF_CHECK(!decode_transaction_record(empty_transaction, decoded).ok());
}

MF_TEST(adversarial, duplicate_and_invalid_requests_are_refused) {
  mftest::TempDir dir("adv-requests");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));

  MaintenanceRequest request;
  request.title = "duplicate targets";
  request.targets = {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 1}};
  request.requestor = requestor_from_name("adversary");
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::InvalidArgument);

  request.targets.clear();
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::InvalidArgument);

  request.targets = {TargetId{TargetKind::Link, 1}};
  request.title.clear();
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::InvalidArgument);

  request.title = std::string(limits::kMaxTitleLength + 1, 'x');
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::InvalidArgument);

  request.title = "oversized target list";
  request.targets.clear();
  for (std::uint32_t i = 0; i <= limits::kMaxTargetsPerJob; ++i) {
    request.targets.push_back(TargetId{TargetKind::Link, i});
  }
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::LimitExceeded);

  request.title = "unknown dependency";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.depends_on = {JobId::from_u64(4242)};
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::NotFound);

  request.depends_on.clear();
  request.targets = {TargetId{TargetKind::Pod, 77}};
  request.title = "unknown target";
  Result<JobId> created = controller->propose(request, "adversary");
  MF_CHECK_OK(created);
  MF_CHECK_OK(controller->approve(created.value(), "adversary"));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));
  MF_CHECK_OK(controller->tick(clock.now()));
  MF_CHECK_EQ(controller->state().jobs.find(created.value())->state, JobState::Blocked);
  MF_CHECK_EQ(controller->state().jobs.find(created.value())->block, BlockReason::TargetUnknown);
}

MF_TEST(adversarial, evidence_replays_and_generation_confusion) {
  mftest::TempDir dir("adv-evidence");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));

  EvidenceRecord record = mftest::health_evidence(TargetId{TargetKind::Link, 1}, Health::Healthy,
                                                  clock.now(), 30 * kNanosPerSecond);
  MF_CHECK_OK(controller->observe(record));
  // Replaying the same observation is refused, so a stale publisher cannot
  // refresh freshness by resending an old value.
  MF_CHECK_ERR(controller->observe(record), ErrorCode::StaleRevision);
  EvidenceRecord regressed = record;
  regressed.revision = Revision::from_u64(1);
  MF_CHECK_ERR(controller->observe(regressed), ErrorCode::StaleRevision);

  // Evidence attributed to a foreign incarnation is stored but is never fresh.
  EvidenceRecord foreign = mftest::health_evidence(TargetId{TargetKind::Link, 2}, Health::Healthy,
                                                   clock.now(), 30 * kNanosPerSecond);
  foreign.observed_by = ControllerIncarnation{ControllerId::from_u64(9), BootEpoch::from_u64(1), 1};
  MF_CHECK_OK(controller->observe(foreign));
  EvidenceQuery query;
  query.key = foreign.key;
  query.current = controller->incarnation();
  query.now = clock.now();
  const Freshness freshness = controller->state().evidence.evaluate(query);
  MF_CHECK(!freshness.fresh());
}

MF_TEST(adversarial, reordered_completion_and_restoration_reports_are_fenced) {
  mftest::TempDir dir("adv-reorder");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));

  MaintenanceRequest request;
  request.title = "reorder";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.requestor = requestor_from_name("adversary");
  Result<JobId> created = controller->propose(request, "adversary");
  MF_CHECK_OK(created);
  const JobId id = created.value();
  MF_CHECK_OK(controller->approve(id, "adversary"));
  for (int i = 0; i < 12; ++i) {
    MF_CHECK_OK(controller->tick(clock.now()));
    clock.advance(kNanosPerSecond);
    if (controller->state().jobs.find(id)->state == JobState::InMaintenance) {
      break;
    }
  }
  const MaintenanceJob& job = *controller->state().jobs.find(id);
  MF_CHECK_EQ(job.state, JobState::InMaintenance);

  // A restoration report before the completion report is accepted as evidence
  // but cannot advance the state machine.
  MF_CHECK_OK(controller->report_restoration(id, job.active_attempt, TargetId{TargetKind::Link, 1},
                                             true, "early"));
  MF_CHECK_EQ(controller->state().jobs.find(id)->state, JobState::InMaintenance);

  // Restoration for a target outside the removal set is refused.
  MF_CHECK_ERR(controller->report_restoration(id, job.active_attempt,
                                              TargetId{TargetKind::Link, 8}, true, "off scope"),
               ErrorCode::InvalidArgument);

  MF_CHECK_OK(controller->report_completion(id, job.active_attempt, true, "done"));
  for (int i = 0; i < 20 && !controller->state().jobs.find(id)->is_terminal(); ++i) {
    MF_CHECK_OK(controller->tick(clock.now()));
    clock.advance(kNanosPerSecond);
  }
  MF_CHECK_EQ(controller->state().jobs.find(id)->state, JobState::Complete);
}

MF_TEST(adversarial, cancelled_operations_never_publish_success) {
  mftest::TempDir dir("adv-cancel");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));
  MF_CHECK_OK(mftest::seed_health(*controller, clock.now()));

  MaintenanceRequest request;
  request.title = "cancel";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.requestor = requestor_from_name("adversary");
  Result<JobId> created = controller->propose(request, "adversary");
  MF_CHECK_OK(created);
  const JobId id = created.value();
  MF_CHECK_OK(controller->approve(id, "adversary"));
  MF_CHECK_OK(controller->cancel(id, "adversary", "withdrawn"));
  for (int i = 0; i < 6; ++i) {
    MF_CHECK_OK(controller->tick(clock.now()));
    clock.advance(kNanosPerSecond);
  }
  const MaintenanceJob& job = *controller->state().jobs.find(id);
  MF_CHECK_EQ(job.state, JobState::Cancelled);
  MF_CHECK(!job.service_removed);
  MF_CHECK(!job.authority.valid());
  MF_CHECK(!job.lease.valid());
  MF_CHECK_EQ(controller->stats().starts, 0u);
  MF_CHECK_EQ(controller->stats().completions, 0u);
  // A cancelled job never restarts even if its targets become admissible.
  MF_CHECK_EQ(controller->state().jobs.find(id)->state, JobState::Cancelled);
}

MF_TEST(adversarial, shutdown_refuses_further_admission) {
  mftest::TempDir dir("adv-shutdown");
  ManualClock clock(1000000000LL);
  auto drain = std::make_shared<LocalDrainPort>();
  RecoveryReport report;
  Result<std::unique_ptr<Controller>> opened =
      mftest::open_controller(dir.file("journal.mfj"), clock, drain);
  MF_CHECK_OK(opened);
  std::unique_ptr<Controller> controller = std::move(opened.value());
  MF_CHECK_OK(mftest::install_fixture(*controller));
  MF_CHECK_OK(controller->shutdown());
  MF_CHECK(controller->shutting_down());
  MaintenanceRequest request;
  request.title = "after shutdown";
  request.targets = {TargetId{TargetKind::Link, 1}};
  request.requestor = requestor_from_name("adversary");
  MF_CHECK_ERR(controller->propose(request, "adversary"), ErrorCode::ShuttingDown);
  MF_CHECK_ERR(controller->tick(clock.now()), ErrorCode::ShuttingDown);
}

MF_TEST_MAIN()
