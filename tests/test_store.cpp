// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "mf/store/journal.hpp"
#include "mf/store/store.hpp"
#include "tests/mf_test.hpp"
#include "tests/support.hpp"

using namespace mf;

namespace {

std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

bool write_file(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return stream.good();
}

StoreOptions options_for(const std::string& path) {
  StoreOptions options;
  options.journal_path = path;
  return options;
}

}  // namespace

MF_TEST(store, fresh_open_creates_a_versioned_journal) {
  mftest::TempDir dir("store-fresh");
  const std::string path = dir.file("journal.mfj");
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  MF_CHECK(report.journal_created);
  MF_CHECK_EQ(report.records_replayed, 0u);
  const std::vector<std::byte> bytes = read_file(path);
  MF_CHECK_EQ(bytes.size(), 12u);
  MF_CHECK_EQ(static_cast<int>(bytes[0]), static_cast<int>(std::byte{'M'}));
}

MF_TEST(store, boot_epoch_strictly_increases) {
  mftest::TempDir dir("store-boot");
  const std::string path = dir.file("journal.mfj");
  BootEpoch first;
  BootEpoch second;
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    Result<ControllerIncarnation> incarnation =
        store.value()->begin_incarnation(ControllerId::from_u64(1), 111);
    MF_CHECK_OK(incarnation);
    first = incarnation.value().boot_epoch;
    MF_CHECK_EQ(first.value(), 1u);
  }
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    MF_CHECK_EQ(report.previous_boot_epoch.value(), 0u);
    Result<ControllerIncarnation> incarnation =
        store.value()->begin_incarnation(ControllerId::from_u64(1), 222);
    MF_CHECK_OK(incarnation);
    MF_CHECK_EQ(store.value()->recovery().previous_boot_epoch.value(), 1u);
    second = incarnation.value().boot_epoch;
    MF_CHECK_EQ(second.value(), 2u);
    MF_CHECK(is_fenced_by((ControllerIncarnation{ControllerId::from_u64(1), first, 111}),
                          (ControllerIncarnation{ControllerId::from_u64(1), second, 222})));
  }
}

MF_TEST(store, state_survives_a_restart) {
  mftest::TempDir dir("store-restart");
  const std::string path = dir.file("journal.mfj");
  JobId job_id;
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    MF_CHECK_OK(store.value()->commit_topology(mftest::make_topology()));
    MF_CHECK_OK(store.value()->commit_policy(mftest::make_policy(2)));
    Result<ControllerIncarnation> incarnation =
        store.value()->begin_incarnation(ControllerId::from_u64(1), 7);
    MF_CHECK_OK(incarnation);

    MaintenanceJob job;
    job.generation = GenerationId::from_u64(1);
    job.title = "restart me";
    job.targets = {TargetId{TargetKind::Link, 1}};
    job.removal_set = job.targets;
    job.requestor = requestor_from_name("test");
    job.owner = incarnation.value();
    Result<JobId> created = store.value()->create_job(std::move(job));
    MF_CHECK_OK(created);
    job_id = created.value();

    EvidenceRecord evidence = mftest::health_evidence(TargetId{TargetKind::Link, 1}, Health::Healthy, 100, 1000);
    evidence.observed_by = incarnation.value();
    MF_CHECK_OK(store.value()->observe_evidence(evidence));

    AuthorityReservation reservation;
    reservation.job = job_id;
    reservation.generation = GenerationId::from_u64(1);
    reservation.attempt = AttemptId{job_id, GenerationId::from_u64(1), AttemptOrdinal::from_u64(1)};
    reservation.owner = incarnation.value();
    reservation.targets = {TargetId{TargetKind::Link, 1}};
    reservation.granted_at = 100;
    reservation.expires_at = 100000;
    MF_CHECK_OK(store.value()->reserve_authority(reservation));
  }
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    MF_CHECK_EQ(report.jobs_recovered, 1u);
    MF_CHECK_EQ(report.evidence_recovered, 1u);
    MF_CHECK_EQ(report.reservations_recovered, 1u);
    MF_CHECK_EQ(store.value()->state().jobs.size(), 1u);
    MF_CHECK_EQ(store.value()->state().topology.targets().size(), 14u);
    MF_CHECK_EQ(store.value()->state().policy.redundancy_for(PoolId::from_u64(1)).tolerated_losses, 2u);
    MF_CHECK(store.value()->state().jobs.next_id() > job_id);
  }
}

MF_TEST(store, evidence_is_not_fresh_after_a_restart) {
  mftest::TempDir dir("store-evidence");
  const std::string path = dir.file("journal.mfj");
  ControllerIncarnation previous;
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    Result<ControllerIncarnation> incarnation =
        store.value()->begin_incarnation(ControllerId::from_u64(1), 5);
    MF_CHECK_OK(incarnation);
    previous = incarnation.value();
    EvidenceRecord evidence = mftest::health_evidence(TargetId{TargetKind::Link, 1}, Health::Healthy, 1000, 1000000);
    evidence.observed_by = previous;
    MF_CHECK_OK(store.value()->observe_evidence(evidence));
  }
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  Result<ControllerIncarnation> incarnation =
      store.value()->begin_incarnation(ControllerId::from_u64(1), 6);
  MF_CHECK_OK(incarnation);

  EvidenceQuery query;
  query.key.kind = EvidenceKind::TargetHealth;
  query.key.subject = EvidenceSubject::of(TargetId{TargetKind::Link, 1});
  query.key.source = SourceId::from_u64(1);
  query.current = incarnation.value();
  query.now = 1100;
  const Freshness freshness = store.value()->state().evidence.evaluate(query);
  MF_CHECK(!freshness.fresh());
  MF_CHECK_EQ(to_string(freshness.state), std::string("foreign-incarnation"));
}

MF_TEST(store, job_revision_fencing) {
  mftest::TempDir dir("store-fence");
  const std::string path = dir.file("journal.mfj");
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  Result<ControllerIncarnation> incarnation =
      store.value()->begin_incarnation(ControllerId::from_u64(1), 1);
  MF_CHECK_OK(incarnation);
  MaintenanceJob job;
  job.generation = GenerationId::from_u64(1);
  job.title = "fence";
  job.targets = {TargetId{TargetKind::Link, 1}};
  job.removal_set = job.targets;
  job.requestor = requestor_from_name("test");
  Result<JobId> created = store.value()->create_job(job);
  MF_CHECK_OK(created);
  const JobId id = created.value();

  MaintenanceJob stale = *store.value()->state().jobs.find(id);
  stale.revision = Revision::from_u64(5);
  MF_CHECK_ERR(store.value()->commit_job(stale), ErrorCode::StaleRevision);

  MaintenanceJob next = *store.value()->state().jobs.find(id);
  next.revision = next.revision.next();
  next.last_detail = "advanced";
  MF_CHECK_OK(store.value()->commit_job(next));
  MF_CHECK_EQ(store.value()->state().jobs.find(id)->revision.value(), 2u);

  MaintenanceJob replay = next;
  MF_CHECK_ERR(store.value()->commit_job(replay), ErrorCode::StaleRevision);
}

MF_TEST(store, compaction_preserves_state_and_shrinks_the_journal) {
  mftest::TempDir dir("store-compact");
  const std::string path = dir.file("journal.mfj");
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  Result<ControllerIncarnation> incarnation =
      store.value()->begin_incarnation(ControllerId::from_u64(1), 1);
  MF_CHECK_OK(incarnation);
  MF_CHECK_OK(store.value()->commit_topology(mftest::make_topology()));
  MF_CHECK_OK(store.value()->commit_policy(mftest::make_policy(1)));

  MaintenanceJob job;
  job.generation = GenerationId::from_u64(1);
  job.title = "churn";
  job.targets = {TargetId{TargetKind::Link, 1}};
  job.removal_set = job.targets;
  job.requestor = requestor_from_name("test");
  Result<JobId> created = store.value()->create_job(job);
  MF_CHECK_OK(created);
  const JobId id = created.value();
  for (int i = 0; i < 40; ++i) {
    MaintenanceJob next = *store.value()->state().jobs.find(id);
    next.revision = next.revision.next();
    next.last_detail = "iteration " + std::to_string(i);
    MF_CHECK_OK(store.value()->commit_job(next));
  }
  const std::uint64_t before = store.value()->journal_records();
  MF_CHECK(before > 40u);
  MF_CHECK_OK(store.value()->compact(2000));
  MF_CHECK(store.value()->journal_records() < before);
  MF_CHECK_EQ(store.value()->state().jobs.find(id)->last_detail, std::string("iteration 39"));
  store.value().reset();

  RecoveryReport reopened;
  Result<std::unique_ptr<Store>> again = Store::open(options_for(path), reopened);
  MF_CHECK_OK(again);
  MF_CHECK(!reopened.truncated);
  MF_CHECK(reopened.records_replayed >= 3u);
  MF_CHECK_EQ(again.value()->state().jobs.size(), 1u);
  MF_CHECK_EQ(again.value()->state().jobs.find(id)->last_detail, std::string("iteration 39"));
}

MF_TEST(store, torn_tail_is_truncated_conservatively) {
  mftest::TempDir dir("store-torn");
  const std::string path = dir.file("journal.mfj");
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    MF_CHECK_OK(store.value()->commit_topology(mftest::make_topology()));
  }
  std::vector<std::byte> bytes = read_file(path);
  const std::size_t good_size = bytes.size();
  for (int i = 0; i < 17; ++i) {
    bytes.push_back(static_cast<std::byte>(0x5A));
  }
  MF_CHECK(write_file(path, bytes));

  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  MF_CHECK(report.truncated);
  MF_CHECK_EQ(report.bytes_recovered, good_size);
  MF_CHECK_EQ(report.truncated_bytes, 17u);
  MF_CHECK_EQ(store.value()->state().topology.targets().size(), 14u);
  MF_CHECK_EQ(read_file(path).size(), good_size);
}

MF_TEST(store, corrupted_payload_stops_replay_before_the_damage) {
  mftest::TempDir dir("store-corrupt");
  const std::string path = dir.file("journal.mfj");
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
    MF_CHECK_OK(store.value()->commit_topology(mftest::make_topology()));
    MF_CHECK_OK(store.value()->commit_policy(mftest::make_policy(1)));
  }
  std::vector<std::byte> bytes = read_file(path);
  // Flip a bit near the end of the file: the first frame survives, the second
  // is discarded and the file is truncated at the first good boundary.
  bytes[bytes.size() - 3] = static_cast<std::byte>(static_cast<unsigned char>(bytes[bytes.size() - 3]) ^ 0xFFu);
  MF_CHECK(write_file(path, bytes));

  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_OK(store);
  MF_CHECK(report.truncated);
  MF_CHECK_EQ(report.frames_replayed, 1u);
  MF_CHECK_EQ(store.value()->state().topology.targets().size(), 14u);
  MF_CHECK_EQ(store.value()->state().policy.redundancy.size(), 0u);
}

MF_TEST(store, corrupt_header_is_a_hard_failure) {
  mftest::TempDir dir("store-header");
  const std::string path = dir.file("journal.mfj");
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
  }
  std::vector<std::byte> bytes = read_file(path);
  bytes[0] = std::byte{'X'};
  MF_CHECK(write_file(path, bytes));
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_ERR(store, ErrorCode::CorruptState);
}

MF_TEST(store, unsupported_journal_version_is_refused) {
  mftest::TempDir dir("store-version");
  const std::string path = dir.file("journal.mfj");
  {
    RecoveryReport report;
    Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
    MF_CHECK_OK(store);
  }
  std::vector<std::byte> bytes = read_file(path);
  bytes[5] = std::byte{99};
  MF_CHECK(write_file(path, bytes));
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_ERR(store, ErrorCode::VersionMismatch);
}

MF_TEST(store, short_file_is_refused) {
  mftest::TempDir dir("store-short");
  const std::string path = dir.file("journal.mfj");
  MF_CHECK(write_file(path, {std::byte{'M'}, std::byte{'F'}}));
  RecoveryReport report;
  Result<std::unique_ptr<Store>> store = Store::open(options_for(path), report);
  MF_CHECK_ERR(store, ErrorCode::CorruptState);
}

MF_TEST(journal, sequence_reordering_is_detected) {
  mftest::TempDir dir("journal-seq");
  const std::string path = dir.file("journal.mfj");
  {
    JournalOptions options;
    options.path = path;
    Result<Journal> journal = Journal::open(options);
    MF_CHECK_OK(journal);
    ReplayStats stats;
    MF_CHECK_OK(journal.value().replay([](const Record&) {}, stats));
    MF_CHECK_OK(journal.value().append(encode_marker_record(1)));
    MF_CHECK_OK(journal.value().append(encode_marker_record(2)));
    MF_CHECK_OK(journal.value().append(encode_marker_record(3)));
  }
  std::vector<std::byte> bytes = read_file(path);
  // Overwrite the sequence number of the third frame with a stale one.
  const std::size_t third = bytes.size() - 8 - 24;
  for (int i = 0; i < 8; ++i) {
    bytes[third + 4 + static_cast<std::size_t>(i)] = std::byte{0};
  }
  bytes[third + 4 + 7] = std::byte{1};
  MF_CHECK(write_file(path, bytes));

  JournalOptions options;
  options.path = path;
  Result<Journal> journal = Journal::open(options);
  MF_CHECK_OK(journal);
  ReplayStats stats;
  std::vector<std::uint64_t> seen;
  MF_CHECK_OK(journal.value().replay(
      [&seen](const Record& record) {
        std::uint64_t value = 0;
        for (const std::byte b : record.payload) {
          value = (value << 8u) | static_cast<std::uint64_t>(static_cast<unsigned char>(b));
        }
        seen.push_back(value);
      },
      stats));
  MF_CHECK(stats.truncated);
  MF_CHECK_EQ(seen.size(), 2u);
  MF_CHECK_EQ(seen[0], 1u);
  MF_CHECK_EQ(seen[1], 2u);
}

MF_TEST(journal, append_requires_replay) {
  mftest::TempDir dir("journal-replay");
  const std::string path = dir.file("journal.mfj");
  {
    JournalOptions options;
    options.path = path;
    Result<Journal> journal = Journal::open(options);
    MF_CHECK_OK(journal);
    ReplayStats stats;
    MF_CHECK_OK(journal.value().replay([](const Record&) {}, stats));
    MF_CHECK_OK(journal.value().append(encode_marker_record(7)));
  }
  JournalOptions options;
  options.path = path;
  Result<Journal> journal = Journal::open(options);
  MF_CHECK_OK(journal);
  MF_CHECK_ERR(journal.value().append(encode_marker_record(8)), ErrorCode::StateConflict);
  ReplayStats stats;
  MF_CHECK_OK(journal.value().replay([](const Record&) {}, stats));
  MF_CHECK_OK(journal.value().append(encode_marker_record(8)));
}

MF_TEST(journal, oversized_records_are_refused) {
  mftest::TempDir dir("journal-big");
  JournalOptions options;
  options.path = dir.file("journal.mfj");
  Result<Journal> journal = Journal::open(options);
  MF_CHECK_OK(journal);
  ReplayStats stats;
  MF_CHECK_OK(journal.value().replay([](const Record&) {}, stats));
  Record huge;
  huge.type = RecordType::Marker;
  huge.payload.resize(limits::kMaxJournalRecordBytes + 1);
  MF_CHECK_ERR(journal.value().append(huge), ErrorCode::LimitExceeded);
  MF_CHECK_EQ(journal.value().records(), 0u);
}

MF_TEST(records, malformed_payloads_are_rejected) {
  Topology topology = mftest::make_topology();
  Record encoded = encode_topology_record(topology);
  Topology decoded;
  MF_CHECK_OK(decode_topology_record(encoded, decoded));
  MF_CHECK_EQ(decoded.digest(), topology.digest());

  Record truncated = encoded;
  truncated.payload.resize(encoded.payload.size() / 2);
  MF_CHECK_ERR(decode_topology_record(truncated, decoded), ErrorCode::CorruptState);

  Record trailing = encoded;
  trailing.payload.push_back(std::byte{0});
  MF_CHECK_ERR(decode_topology_record(trailing, decoded), ErrorCode::CorruptState);

  Record wrong_layout = encoded;
  wrong_layout.payload[1] = std::byte{9};
  MF_CHECK_ERR(decode_topology_record(wrong_layout, decoded), ErrorCode::VersionMismatch);

  Record wrong_type = encoded;
  wrong_type.type = RecordType::Policy;
  MF_CHECK_ERR(decode_topology_record(wrong_type, decoded), ErrorCode::InvalidArgument);

  // A declared count larger than the bound is rejected before allocation.
  // The domain count lives at bytes 6..9, after the layout and revision.
  Record absurd = encoded;
  absurd.payload[6] = std::byte{0xFF};
  absurd.payload[7] = std::byte{0xFF};
  MF_CHECK_ERR(decode_topology_record(absurd, decoded), ErrorCode::CorruptState);
}

MF_TEST_MAIN()
