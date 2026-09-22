// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/store/store.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

#include "mf/core/limits.hpp"
#include "mf/version.hpp"

namespace mf {
namespace {

Nanos system_nonce() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return static_cast<Nanos>(count == 0 ? 1 : count);
}

}  // namespace

std::string RecoveryReport::to_text() const {
  std::string out = "recovery: frames=" + std::to_string(frames_replayed) +
                    " records=" + std::to_string(records_replayed) +
                    " discarded=" + std::to_string(discarded_records) +
                    " bytes=" + std::to_string(bytes_recovered);
  if (truncated) {
    out += " truncated=" + std::to_string(truncated_bytes) + " bytes";
  }
  out += " previous-boot=" + to_string(previous_boot_epoch) + " boot=" + to_string(boot_epoch);
  out += " jobs=" + std::to_string(jobs_recovered) + " reservations=" +
         std::to_string(reservations_recovered) + " leases=" + std::to_string(leases_recovered) +
         " quarantines=" + std::to_string(quarantines_recovered) + " evidence=" +
         std::to_string(evidence_recovered);
  if (!detail.empty()) {
    out += " -- " + detail;
  }
  return out;
}

Store::Store(Journal journal, StoreOptions options)
    : journal_(std::move(journal)), options_(std::move(options)) {
  state_.evidence = EvidenceStore(options_.evidence_capacity);
  state_.ledger = AuthorityLedger(options_.authority_lease);
}

Store::~Store() = default;

Status Store::apply(const Record& record, bool strict) {
  switch (record.type) {
    case RecordType::Topology: {
      Topology topology;
      const Status status = decode_topology_record(record, topology);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      state_.topology = std::move(topology);
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Policy: {
      Policy policy;
      const Status status = decode_policy_record(record, policy);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      state_.policy = std::move(policy);
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Job: {
      MaintenanceJob job;
      const Status status = decode_job_record(record, job);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      const Status adopted = state_.jobs.adopt(std::move(job));
      if (!adopted.ok()) {
        ++report_.discarded_records;
        return ok_status();
      }
      ++report_.jobs_recovered;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::JobErased: {
      JobId id;
      const Status status = decode_job_erased_record(record, id);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      (void)state_.jobs.erase(id);
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Evidence: {
      EvidenceRecord evidence;
      const Status status = decode_evidence_record(record, evidence);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      const Status observed = state_.evidence.observe(std::move(evidence));
      if (!observed.ok()) {
        ++report_.discarded_records;
        return ok_status();
      }
      ++report_.evidence_recovered;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Reservation: {
      AuthorityReservation reservation;
      const Status status = decode_reservation_record(record, reservation);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      const Status adopted = state_.ledger.adopt(std::move(reservation));
      if (!adopted.ok()) {
        ++report_.discarded_records;
        return ok_status();
      }
      ++report_.reservations_recovered;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::ReservationErased: {
      AuthorityId id;
      const Status status = decode_reservation_erased_record(record, id);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      (void)state_.ledger.erase(id);
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Lease: {
      DrainLease lease;
      const Status status = decode_lease_record(record, lease);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      state_.leases[lease.id] = std::move(lease);
      ++report_.leases_recovered;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::LeaseErased: {
      DrainLeaseId id;
      const Status status = decode_lease_erased_record(record, id);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      state_.leases.erase(id);
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Quarantine: {
      QuarantineRecord quarantine;
      const Status status = decode_quarantine_record(record, quarantine);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      const Status adopted = state_.quarantines.adopt(std::move(quarantine));
      if (!adopted.ok()) {
        ++report_.discarded_records;
        return ok_status();
      }
      ++report_.quarantines_recovered;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Boot: {
      ControllerIncarnation incarnation;
      BootEpoch prior;
      const Status status = decode_boot_record(record, incarnation, prior);
      if (!status.ok()) {
        if (strict) {
          return status;
        }
        ++report_.discarded_records;
        return ok_status();
      }
      state_.last_incarnation = incarnation;
      state_.boot_epoch = incarnation.boot_epoch;
      ++report_.records_replayed;
      return ok_status();
    }
    case RecordType::Marker:
      ++report_.records_replayed;
      return ok_status();
    case RecordType::Transaction:
    case RecordType::Unknown:
      break;
  }
  ++report_.discarded_records;
  return ok_status();
}

Result<std::unique_ptr<Store>> Store::open(const StoreOptions& options, RecoveryReport& report) {
  JournalOptions journal_options;
  journal_options.path = options.journal_path;
  journal_options.create_if_missing = options.create_if_missing;

  const bool existed = std::filesystem::exists(options.journal_path);
  Result<Journal> opened = Journal::open(journal_options);
  if (!opened.ok()) {
    return opened.error();
  }
  Journal journal = std::move(opened.value());
  std::unique_ptr<Store> store(new Store(std::move(journal), options));
  store->report_.journal_created = !existed;
  store->report_.format_version = kStateFormatVersion;

  ReplayStats stats;
  // Structural records are strict: a topology or policy that cannot be decoded
  // means the runtime cannot know what it is protecting.
  const Status replayed = store->journal_.replay(
      [&store](const Record& record) {
        const bool strict = record.type == RecordType::Topology ||
                            record.type == RecordType::Policy;
        const Status applied = store->apply(record, strict);
        if (!applied.ok()) {
          store->report_.detail =
              std::string("replay rejected a ") + to_string(record.type) + " record: " +
              applied.error().detail;
        }
      },
      stats);
  if (!replayed.ok()) {
    return replayed.error();
  }
  store->report_.frames_replayed = stats.frames;
  store->report_.records_replayed = stats.records;
  store->report_.bytes_recovered = stats.bytes;
  store->report_.truncated = stats.truncated;
  store->report_.truncated_bytes = stats.truncated_bytes;
  if (!stats.detail.empty()) {
    store->report_.detail = stats.detail;
  }

  // Id allocators must never hand out an identity that has already been used.
  JobId next_job = JobId::from_u64(1);
  for (const auto& [id, job] : store->state_.jobs.jobs()) {
    (void)job;
    if (id >= next_job) {
      next_job = id.next();
    }
  }
  store->state_.jobs.set_next_id(next_job);
  report = store->report_;
  return store;
}

Result<ControllerIncarnation> Store::begin_incarnation(ControllerId controller, std::uint64_t nonce) {
  if (!controller.valid()) {
    return invalid_argument("controller id is not valid");
  }
  const BootEpoch prior = state_.boot_epoch;
  const BootEpoch epoch = prior.valid() ? prior.next() : BootEpoch::from_u64(1);
  ControllerIncarnation incarnation;
  incarnation.controller = controller;
  incarnation.boot_epoch = epoch;
  incarnation.nonce = nonce == 0 ? static_cast<std::uint64_t>(system_nonce()) : nonce;

  const Record record = encode_boot_record(incarnation, prior);
  const Status appended = journal_.append_transaction({record});
  if (!appended.ok()) {
    return appended.error();
  }
  const Status synced = journal_.flush();
  if (!synced.ok()) {
    return synced.error();
  }
  state_.last_incarnation = incarnation;
  state_.boot_epoch = epoch;
  report_.previous_boot_epoch = prior;
  report_.boot_epoch = epoch;
  return incarnation;
}

Status Store::append_and_apply(const std::vector<Record>& records) {
  for (const Record& record : records) {
    const Status valid = validate_record(record);
    if (!valid.ok()) {
      return valid;
    }
  }
  const Status appended = journal_.append_transaction(records);
  if (!appended.ok()) {
    return appended;
  }
  const Status synced = journal_.flush();
  if (!synced.ok()) {
    return synced;
  }
  for (const Record& record : records) {
    const Status applied = apply(record, true);
    if (!applied.ok()) {
      return make_error(ErrorCode::Internal,
                        std::string("journal accepted a record the store could not apply: ") +
                            applied.error().detail);
    }
  }
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::commit_topology(Topology topology) {
  Topology validated = std::move(topology);
  const Status valid = validated.validate();
  if (!valid.ok()) {
    return valid;
  }
  return append_and_apply({encode_topology_record(validated)});
}

Status Store::commit_policy(Policy policy) {
  Policy validated = std::move(policy);
  const Status valid = validated.validate();
  if (!valid.ok()) {
    return valid;
  }
  return append_and_apply({encode_policy_record(validated)});
}

Result<JobId> Store::create_job(MaintenanceJob job) {
  if (state_.jobs.size() >= limits::kMaxJobsTotal) {
    return make_error(ErrorCode::LimitExceeded, "job table is at capacity");
  }
  job.id = state_.jobs.allocate_id();
  job.revision = Revision::from_u64(1);
  const Status inserted = state_.jobs.insert(job);
  if (!inserted.ok()) {
    return inserted.error();
  }
  const Status appended = append_and_apply({encode_job_record(job)});
  if (!appended.ok()) {
    (void)state_.jobs.erase(job.id);
    return appended.error();
  }
  return job.id;
}

EvidenceId Store::allocate_evidence_id() {
  ++evidence_sequence_;
  const std::uint64_t epoch = state_.last_incarnation.boot_epoch.valid()
                                  ? state_.last_incarnation.boot_epoch.value()
                                  : 1;
  return EvidenceId::from_u64((epoch << 40u) | (evidence_sequence_ & 0xFFFFFFFFFFull));
}

Result<QuarantineId> Store::quarantine_target(TargetId target, JobId job, GenerationId generation,
                                              BlockReason reason, std::string detail, Nanos now,
                                              const ControllerIncarnation& by) {
  QuarantineRecord record;
  record.id = state_.quarantines.allocate_id();
  record.target = target;
  record.job = job;
  record.generation = generation;
  record.reason = reason;
  record.detail = std::move(detail);
  record.created_at = now;
  record.created_by = by;
  record.digest = quarantine_digest(record);
  QuarantineTable staging = state_.quarantines;
  const Status inserted = staging.insert(record);
  if (!inserted.ok()) {
    // Roll the allocator back so identities are never burned on failure.
    staging.set_next_id(record.id);
    return inserted.error();
  }
  const Status appended = journal_.append_transaction({encode_quarantine_record(record)});
  if (!appended.ok()) {
    return appended.error();
  }
  state_.quarantines = std::move(staging);
  state_.revision = state_.revision.next();
  return record.id;
}

Status Store::commit_job(MaintenanceJob job) {
  const MaintenanceJob* existing = state_.jobs.find(job.id);
  const bool had_existing = existing != nullptr;
  const MaintenanceJob previous = had_existing ? *existing : MaintenanceJob{};
  const Status fenced = state_.jobs.put(job);
  if (!fenced.ok()) {
    return fenced;
  }
  const Status appended = append_and_apply({encode_job_record(job)});
  if (!appended.ok()) {
    // The journal refused the write: undo the in-memory change so memory and
    // disk cannot diverge.
    if (had_existing) {
      (void)state_.jobs.adopt(previous);
    } else {
      (void)state_.jobs.erase(job.id);
    }
    return appended;
  }
  return ok_status();
}

Status Store::commit_jobs(const std::vector<MaintenanceJob>& jobs) {
  if (jobs.empty()) {
    return invalid_argument("no jobs supplied");
  }
  std::vector<MaintenanceJob> previous;
  previous.reserve(jobs.size());
  std::vector<Record> records;
  records.reserve(jobs.size());
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    const MaintenanceJob& job = jobs[i];
    const MaintenanceJob* existing = state_.jobs.find(job.id);
    if (existing == nullptr) {
      for (std::size_t j = 0; j < i; ++j) {
        (void)state_.jobs.adopt(previous[j]);
      }
      return make_error(ErrorCode::NotFound, "job " + to_string(job.id) + " does not exist");
    }
    previous.push_back(*existing);
    const Status fenced = state_.jobs.put(job);
    if (!fenced.ok()) {
      for (std::size_t j = 0; j <= i; ++j) {
        (void)state_.jobs.adopt(previous[j]);
      }
      return fenced;
    }
    records.push_back(encode_job_record(job));
  }
  const Status appended = append_and_apply(records);
  if (!appended.ok()) {
    for (const MaintenanceJob& rollback : previous) {
      (void)state_.jobs.adopt(rollback);
    }
    return appended;
  }
  return ok_status();
}

Status Store::commit_job_erased(JobId job) {
  const MaintenanceJob* existing = state_.jobs.find(job);
  if (existing == nullptr) {
    return make_error(ErrorCode::NotFound, "job " + to_string(job) + " does not exist");
  }
  const MaintenanceJob previous = *existing;
  const Status erased = state_.jobs.erase(job);
  if (!erased.ok()) {
    return erased;
  }
  const Status appended = append_and_apply({encode_job_erased_record(job)});
  if (!appended.ok()) {
    (void)state_.jobs.adopt(previous);
    return appended;
  }
  return ok_status();
}

Status Store::observe_evidence(EvidenceRecord evidence) {
  EvidenceStore staging = state_.evidence;
  const Status observed = staging.observe(evidence);
  if (!observed.ok()) {
    return observed;
  }
  const Status appended = journal_.append_transaction({encode_evidence_record(evidence)});
  if (!appended.ok()) {
    return appended;
  }
  state_.evidence = std::move(staging);
  state_.revision = state_.revision.next();
  return ok_status();
}

Result<AuthorityId> Store::reserve_authority(AuthorityReservation reservation) {
  AuthorityLedger staging = state_.ledger;
  Result<AuthorityId> granted = staging.reserve(std::move(reservation));
  if (!granted.ok()) {
    return granted.error();
  }
  const AuthorityReservation* stored = staging.find(granted.value());
  if (stored == nullptr) {
    return internal_error("authority ledger lost a reservation it just granted");
  }
  const Status appended = journal_.append_transaction({encode_reservation_record(*stored)});
  if (!appended.ok()) {
    return appended.error();
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return granted.value();
}

Status Store::insert_authority(const AuthorityReservation& reservation) {
  AuthorityLedger staging = state_.ledger;
  const Status adopted = staging.adopt(reservation);
  if (!adopted.ok()) {
    return adopted;
  }
  const Status appended = journal_.append_transaction({encode_reservation_record(reservation)});
  if (!appended.ok()) {
    return appended;
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::release_authority(AuthorityId id, const ControllerIncarnation& current) {
  AuthorityLedger staging = state_.ledger;
  const Status released = staging.release(id, current);
  if (!released.ok()) {
    return released;
  }
  const Status appended = journal_.append_transaction({encode_reservation_erased_record(id)});
  if (!appended.ok()) {
    return appended;
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::renew_authority(AuthorityId id, const AttemptId& attempt,
                              const ControllerIncarnation& current, Nanos extend, Nanos now) {
  AuthorityLedger staging = state_.ledger;
  const Status renewed = staging.renew(id, attempt, current, extend, now);
  if (!renewed.ok()) {
    return renewed;
  }
  const AuthorityReservation* stored = staging.find(id);
  if (stored == nullptr) {
    return internal_error("authority ledger lost a reservation it just renewed");
  }
  const Status appended = journal_.append_transaction({encode_reservation_record(*stored)});
  if (!appended.ok()) {
    return appended;
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return ok_status();
}

std::size_t Store::expire_authority(Nanos now) {
  std::vector<AuthorityId> expired;
  for (const auto& [id, reservation] : state_.ledger.reservations()) {
    if (!reservation.live_at(now)) {
      expired.push_back(id);
    }
  }
  if (expired.empty()) {
    return 0;
  }
  std::vector<Record> records;
  records.reserve(expired.size());
  for (const AuthorityId id : expired) {
    records.push_back(encode_reservation_erased_record(id));
  }
  AuthorityLedger staging = state_.ledger;
  const std::size_t removed = staging.expire(now);
  const Status appended = journal_.append_transaction(records);
  if (!appended.ok()) {
    return 0;
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return removed;
}

std::vector<AuthorityReservation> Store::fence_authority(const ControllerIncarnation& current) {
  AuthorityLedger staging = state_.ledger;
  std::vector<AuthorityReservation> revoked = staging.fence_older_incarnations(current);
  if (revoked.empty()) {
    return revoked;
  }
  std::vector<Record> records;
  records.reserve(revoked.size());
  for (const AuthorityReservation& reservation : revoked) {
    records.push_back(encode_reservation_erased_record(reservation.id));
  }
  const Status appended = journal_.append_transaction(records);
  if (!appended.ok()) {
    return {};
  }
  state_.ledger = std::move(staging);
  state_.revision = state_.revision.next();
  return revoked;
}

Status Store::commit_lease(DrainLease lease) {
  const Status appended = journal_.append_transaction({encode_lease_record(lease)});
  if (!appended.ok()) {
    return appended;
  }
  state_.leases[lease.id] = std::move(lease);
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::commit_lease_erased(DrainLeaseId id) {
  const Status appended = journal_.append_transaction({encode_lease_erased_record(id)});
  if (!appended.ok()) {
    return appended;
  }
  state_.leases.erase(id);
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::commit_quarantine(QuarantineRecord record) {
  QuarantineTable staging = state_.quarantines;
  Status applied = staging.find(record.id) == nullptr ? staging.insert(record) : staging.put(record);
  if (!applied.ok()) {
    return applied;
  }
  const Status appended = journal_.append_transaction({encode_quarantine_record(record)});
  if (!appended.ok()) {
    return appended;
  }
  state_.quarantines = std::move(staging);
  state_.revision = state_.revision.next();
  return ok_status();
}

Status Store::flush() { return journal_.flush(); }

bool Store::should_compact() const noexcept {
  return journal_.records() >= options_.compaction_threshold_records;
}

Status Store::compact(Nanos now) {
  std::vector<Record> records;
  records.push_back(encode_topology_record(state_.topology));
  records.push_back(encode_policy_record(state_.policy));
  for (const auto& [id, job] : state_.jobs.jobs()) {
    (void)id;
    records.push_back(encode_job_record(job));
  }
  for (const auto& [id, reservation] : state_.ledger.reservations()) {
    (void)id;
    records.push_back(encode_reservation_record(reservation));
  }
  for (const auto& [id, lease] : state_.leases) {
    (void)id;
    records.push_back(encode_lease_record(lease));
  }
  for (const auto& [id, quarantine] : state_.quarantines.records()) {
    (void)id;
    records.push_back(encode_quarantine_record(quarantine));
  }
  for (const auto& [key, evidence] : state_.evidence.records()) {
    (void)key;
    // Expired volatile observations are already unusable and would only consume
    // the journal bound after a restart; they are dropped at compaction.
    if (evidence.klass == EvidenceClass::Volatile &&
        now > evidence.observed_at + evidence.ttl) {
      continue;
    }
    records.push_back(encode_evidence_record(evidence));
  }
  if (state_.last_incarnation.valid()) {
    records.push_back(
        encode_boot_record(state_.last_incarnation, report_.previous_boot_epoch));
  }
  if (records.size() > limits::kMaxJournalRecords) {
    return make_error(ErrorCode::LimitExceeded, "compacted state exceeds the journal record bound");
  }
  const Status rewritten = journal_.rewrite(records);
  if (!rewritten.ok()) {
    return rewritten;
  }
  ++compactions_;
  return ok_status();
}

}  // namespace mf
