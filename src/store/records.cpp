// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/store/records.hpp"

#include <algorithm>

#include "mf/core/checked.hpp"
#include "mf/core/limits.hpp"

namespace mf {
namespace {

constexpr std::uint16_t kTopologyLayout = 1;
constexpr std::uint16_t kPolicyLayout = 1;
constexpr std::uint16_t kJobLayout = 1;
constexpr std::uint16_t kEvidenceLayout = 1;
constexpr std::uint16_t kReservationLayout = 1;
constexpr std::uint16_t kLeaseLayout = 1;
constexpr std::uint16_t kQuarantineLayout = 1;
constexpr std::uint16_t kBootLayout = 1;
constexpr std::uint16_t kTransactionLayout = 1;

void write_target_id(ByteWriter& writer, TargetId id) {
  writer.u32(static_cast<std::uint32_t>(id.kind));
  writer.u32(id.index);
}

TargetId read_target_id(ByteReader& reader) {
  const std::uint32_t kind = reader.u32();
  const std::uint32_t index = reader.u32();
  TargetId id;
  id.kind = kind < kTargetKindCount ? static_cast<TargetKind>(kind) : TargetKind::Unknown;
  id.index = index;
  return id;
}

void write_domain_id(ByteWriter& writer, DomainId id) {
  writer.u32(static_cast<std::uint32_t>(id.kind));
  writer.u32(id.index);
}

DomainId read_domain_id(ByteReader& reader) {
  const std::uint32_t kind = reader.u32();
  const std::uint32_t index = reader.u32();
  DomainId id;
  if (kind < kDomainKindCount) {
    id.kind = static_cast<DomainKind>(kind);
  } else {
    id.kind = DomainKind::Unknown;
  }
  id.index = index;
  return id;
}

void write_target_list(ByteWriter& writer, const std::vector<TargetId>& targets) {
  writer.u32(static_cast<std::uint32_t>(targets.size()));
  for (const TargetId id : targets) {
    write_target_id(writer, id);
  }
}

Status read_target_list(ByteReader& reader, std::vector<TargetId>& out, std::size_t limit) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok() || static_cast<std::size_t>(count) > limit) {
    return make_error(ErrorCode::CorruptState, "target list exceeds the accepted bound");
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TargetId id = read_target_id(reader);
    if (!reader.ok() || !id.valid()) {
      return make_error(ErrorCode::CorruptState, "target list contains an invalid identity");
    }
    out.push_back(id);
  }
  return ok_status();
}

Status read_domain_list(ByteReader& reader, std::vector<DomainId>& out, std::size_t limit) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok() || static_cast<std::size_t>(count) > limit) {
    return make_error(ErrorCode::CorruptState, "domain list exceeds the accepted bound");
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const DomainId id = read_domain_id(reader);
    if (!reader.ok() || !id.valid()) {
      return make_error(ErrorCode::CorruptState, "domain list contains an invalid identity");
    }
    out.push_back(id);
  }
  return ok_status();
}

void write_attempt_id(ByteWriter& writer, const AttemptId& id) {
  writer.u64(id.job.value());
  writer.u64(id.generation.value());
  writer.u64(id.ordinal.value());
}

AttemptId read_attempt_id(ByteReader& reader) {
  AttemptId id;
  id.job = JobId::from_u64(reader.u64());
  id.generation = GenerationId::from_u64(reader.u64());
  id.ordinal = AttemptOrdinal::from_u64(reader.u64());
  return id;
}

void write_incarnation(ByteWriter& writer, const ControllerIncarnation& incarnation) {
  writer.u64(incarnation.controller.value());
  writer.u64(incarnation.boot_epoch.value());
  writer.u64(incarnation.nonce);
}

ControllerIncarnation read_incarnation(ByteReader& reader) {
  ControllerIncarnation incarnation;
  incarnation.controller = ControllerId::from_u64(reader.u64());
  incarnation.boot_epoch = BootEpoch::from_u64(reader.u64());
  incarnation.nonce = reader.u64();
  return incarnation;
}

Status check_layout(ByteReader& reader, std::uint16_t expected, const char* what) {
  const std::uint16_t layout = reader.u16();
  if (!reader.ok()) {
    return make_error(ErrorCode::CorruptState, std::string(what) + " record is truncated");
  }
  if (layout != expected) {
    return make_error(ErrorCode::VersionMismatch,
                      std::string(what) + " record layout " + std::to_string(layout) +
                          " is not supported (expected " + std::to_string(expected) + ")");
  }
  return ok_status();
}

}  // namespace

const char* to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::Unknown: return "unknown";
    case RecordType::Transaction: return "transaction";
    case RecordType::Topology: return "topology";
    case RecordType::Policy: return "policy";
    case RecordType::Job: return "job";
    case RecordType::Evidence: return "evidence";
    case RecordType::Reservation: return "reservation";
    case RecordType::Lease: return "lease";
    case RecordType::Quarantine: return "quarantine";
    case RecordType::Boot: return "boot";
    case RecordType::JobErased: return "job-erased";
    case RecordType::ReservationErased: return "reservation-erased";
    case RecordType::LeaseErased: return "lease-erased";
    case RecordType::QuarantineErased: return "quarantine-erased";
    case RecordType::Marker: return "marker";
  }
  return "unknown";
}

bool is_known_record_type(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(RecordType::Transaction) &&
         value <= static_cast<std::uint16_t>(RecordType::Marker);
}

Status validate_record(const Record& record) {
  if (record.type == RecordType::Unknown || !is_known_record_type(static_cast<std::uint16_t>(record.type))) {
    return make_error(ErrorCode::CorruptState, "record has an unrecognised type");
  }
  if (record.payload.size() > limits::kMaxJournalRecordBytes) {
    return make_error(ErrorCode::LimitExceeded, "record payload exceeds the journal bound");
  }
  return ok_status();
}

Record encode_topology_record(const Topology& topology) {
  ByteWriter writer;
  writer.u16(kTopologyLayout);
  writer.u32(static_cast<std::uint32_t>(topology.revision().value()));
  writer.u32(static_cast<std::uint32_t>(topology.domains().size()));
  for (const auto& [id, record] : topology.domains()) {
    write_domain_id(writer, id);
    writer.str(record.name, limits::kMaxNameLength);
    writer.boolean(record.correlated);
  }
  writer.u32(static_cast<std::uint32_t>(topology.targets().size()));
  // Containment order: a child record is only decodable once its parent exists.
  std::vector<std::pair<std::size_t, TargetId>> ordered;
  ordered.reserve(topology.targets().size());
  for (const auto& [id, record] : topology.targets()) {
    (void)record;
    ordered.emplace_back(topology.ancestors(id).size(), id);
  }
  std::sort(ordered.begin(), ordered.end());
  for (const auto& [depth, id] : ordered) {
    (void)depth;
    const TargetRecord& record = topology.targets().at(id);
    write_target_id(writer, id);
    writer.str(record.name, limits::kMaxNameLength);
    writer.boolean(record.parent.has_value());
    if (record.parent.has_value()) {
      write_target_id(writer, *record.parent);
    }
    writer.u32(static_cast<std::uint32_t>(record.domains.size()));
    for (const DomainId domain : record.domains) {
      write_domain_id(writer, domain);
    }
    writer.u32(record.capacity_units);
    writer.boolean(record.maintainable);
  }
  writer.u32(static_cast<std::uint32_t>(topology.pools().size()));
  for (const auto& [id, record] : topology.pools()) {
    writer.u64(id.value());
    writer.str(record.name, limits::kMaxNameLength);
    write_target_list(writer, record.members);
  }
  Record out;
  out.type = RecordType::Topology;
  out.payload = writer.take();
  return out;
}

Status decode_topology_record(const Record& record, Topology& topology) {
  if (record.type != RecordType::Topology) {
    return invalid_argument("record is not a topology record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kTopologyLayout, "topology");
  if (!layout.ok()) {
    return layout;
  }
  const std::uint32_t stored_revision = reader.u32();
  (void)stored_revision;
  Topology fresh;
  const std::uint32_t domain_count = reader.u32();
  if (!reader.ok() || domain_count > limits::kMaxDomainsPerTopology) {
    return make_error(ErrorCode::CorruptState, "topology domain count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    DomainRecord domain;
    domain.id = read_domain_id(reader);
    domain.name = reader.str(limits::kMaxNameLength);
    domain.correlated = reader.boolean();
    if (!reader.ok() || !domain.id.valid()) {
      return make_error(ErrorCode::CorruptState, "topology record is malformed");
    }
    const Status added = fresh.add_domain(std::move(domain));
    if (!added.ok()) {
      return make_error(ErrorCode::CorruptState, added.error().detail);
    }
  }
  const std::uint32_t target_count = reader.u32();
  if (!reader.ok() || target_count > limits::kMaxTargetsPerTopology) {
    return make_error(ErrorCode::CorruptState, "topology target count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    TargetRecord target;
    target.id = read_target_id(reader);
    target.name = reader.str(limits::kMaxNameLength);
    if (reader.boolean()) {
      target.parent = read_target_id(reader);
    }
    const Status domains = read_domain_list(reader, target.domains, limits::kMaxDomainsPerTarget);
    if (!domains.ok()) {
      return domains;
    }
    target.capacity_units = reader.u32();
    target.maintainable = reader.boolean();
    if (!reader.ok() || !target.id.valid()) {
      return make_error(ErrorCode::CorruptState, "topology record is malformed");
    }
    const Status added = fresh.add_target(std::move(target));
    if (!added.ok()) {
      return make_error(ErrorCode::CorruptState, added.error().detail);
    }
  }
  const std::uint32_t pool_count = reader.u32();
  if (!reader.ok() || pool_count > limits::kMaxPoolsPerTopology) {
    return make_error(ErrorCode::CorruptState, "topology pool count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < pool_count; ++i) {
    PoolRecord pool;
    pool.id = PoolId::from_u64(reader.u64());
    pool.name = reader.str(limits::kMaxNameLength);
    const Status members = read_target_list(reader, pool.members, limits::kMaxMembersPerPool);
    if (!members.ok()) {
      return members;
    }
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "topology record is malformed");
    }
    const Status added = fresh.add_pool(std::move(pool));
    if (!added.ok()) {
      return make_error(ErrorCode::CorruptState, added.error().detail);
    }
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "topology record has trailing bytes");
  }
  const Status valid = fresh.validate();
  if (!valid.ok()) {
    return valid;
  }
  topology = std::move(fresh);
  return ok_status();
}

Record encode_policy_record(const Policy& policy) {
  ByteWriter writer;
  writer.u16(kPolicyLayout);
  writer.u32(static_cast<std::uint32_t>(policy.revision.value()));

  writer.u32(static_cast<std::uint32_t>(policy.redundancy.size()));
  for (const RedundancyRule& rule : policy.redundancy) {
    writer.u64(rule.pool.value());
    writer.u32(rule.min_viable);
    writer.u32(rule.tolerated_losses);
    writer.str(rule.name, limits::kMaxNameLength);
  }
  writer.u32(static_cast<std::uint32_t>(policy.domain_limits.size()));
  for (const DomainLimitRule& rule : policy.domain_limits) {
    write_domain_id(writer, rule.domain);
    writer.u32(rule.max_out);
    writer.str(rule.name, limits::kMaxNameLength);
  }
  writer.u32(static_cast<std::uint32_t>(policy.headroom.size()));
  for (const HeadroomRule& rule : policy.headroom) {
    writer.u64(rule.pool.value());
    writer.u32(rule.reserve_units);
    writer.str(rule.name, limits::kMaxNameLength);
  }
  writer.u32(static_cast<std::uint32_t>(policy.contracts.size()));
  for (const Contract& contract : policy.contracts) {
    writer.u64(contract.id.value());
    writer.str(contract.name, limits::kMaxNameLength);
    write_target_list(writer, contract.members);
    writer.u32(contract.min_available);
    writer.boolean(contract.require_fresh_evidence);
  }
  writer.u32(static_cast<std::uint32_t>(policy.windows.size()));
  for (const Window& window : policy.windows) {
    writer.u64(window.id.value());
    writer.str(window.name, limits::kMaxNameLength);
    writer.i64(window.opens_at);
    writer.i64(window.closes_at);
    writer.i64(window.min_lead_time);
    writer.i64(window.abort_grace);
    writer.u32(static_cast<std::uint32_t>(window.on_close));
    write_target_list(writer, window.targets);
  }
  writer.u32(static_cast<std::uint32_t>(policy.tiers.size()));
  for (const PriorityTier& tier : policy.tiers) {
    writer.u32(tier.index);
    writer.str(tier.name, limits::kMaxNameLength);
  }
  writer.u32(static_cast<std::uint32_t>(policy.evidence_ttl.size()));
  for (const auto& [kind, ttl] : policy.evidence_ttl) {
    writer.u32(static_cast<std::uint32_t>(kind));
    writer.i64(ttl);
  }
  writer.u32(policy.min_evidence_sources);
  writer.boolean(policy.require_window);
  writer.i64(policy.max_precondition_age);
  writer.i64(policy.default_duration);
  writer.u32(policy.max_concurrent_jobs);
  writer.i64(policy.authority_lease);
  writer.i64(policy.drain_lease);
  writer.u32(policy.max_drain_failures_before_block);
  writer.u32(policy.max_verification_attempts);
  writer.u32(policy.max_restoration_attempts);
  Record out;
  out.type = RecordType::Policy;
  out.payload = writer.take();
  return out;
}

Status decode_policy_record(const Record& record, Policy& policy) {
  if (record.type != RecordType::Policy) {
    return invalid_argument("record is not a policy record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kPolicyLayout, "policy");
  if (!layout.ok()) {
    return layout;
  }
  Policy fresh;
  fresh.revision = Revision::from_u64(reader.u32());

  std::uint32_t count = reader.u32();
  if (!reader.ok() || count > limits::kMaxRedundancyRules) {
    return make_error(ErrorCode::CorruptState, "policy redundancy count exceeds the bound");
  }
  fresh.redundancy.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    RedundancyRule rule;
    rule.pool = PoolId::from_u64(reader.u64());
    rule.min_viable = reader.u32();
    rule.tolerated_losses = reader.u32();
    rule.name = reader.str(limits::kMaxNameLength);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.redundancy.push_back(std::move(rule));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxDomainLimitRules) {
    return make_error(ErrorCode::CorruptState, "policy domain-limit count exceeds the bound");
  }
  fresh.domain_limits.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    DomainLimitRule rule;
    rule.domain = read_domain_id(reader);
    rule.max_out = reader.u32();
    rule.name = reader.str(limits::kMaxNameLength);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.domain_limits.push_back(std::move(rule));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxRedundancyRules) {
    return make_error(ErrorCode::CorruptState, "policy headroom count exceeds the bound");
  }
  fresh.headroom.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    HeadroomRule rule;
    rule.pool = PoolId::from_u64(reader.u64());
    rule.reserve_units = reader.u32();
    rule.name = reader.str(limits::kMaxNameLength);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.headroom.push_back(std::move(rule));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxContracts) {
    return make_error(ErrorCode::CorruptState, "policy contract count exceeds the bound");
  }
  fresh.contracts.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Contract contract;
    contract.id = ContractId::from_u64(reader.u64());
    contract.name = reader.str(limits::kMaxNameLength);
    const Status members = read_target_list(reader, contract.members, limits::kMaxMembersPerPool);
    if (!members.ok()) {
      return members;
    }
    contract.min_available = reader.u32();
    contract.require_fresh_evidence = reader.boolean();
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.contracts.push_back(std::move(contract));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxWindowRules) {
    return make_error(ErrorCode::CorruptState, "policy window count exceeds the bound");
  }
  fresh.windows.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Window window;
    window.id = WindowId::from_u64(reader.u64());
    window.name = reader.str(limits::kMaxNameLength);
    window.opens_at = reader.i64();
    window.closes_at = reader.i64();
    window.min_lead_time = reader.i64();
    window.abort_grace = reader.i64();
    const std::uint32_t on_close = reader.u32();
    if (on_close > static_cast<std::uint32_t>(OnWindowClose::AbortAndRestore)) {
      return make_error(ErrorCode::CorruptState, "policy window has an unknown close policy");
    }
    window.on_close = static_cast<OnWindowClose>(on_close);
    const Status targets = read_target_list(reader, window.targets, limits::kMaxWindowTargets);
    if (!targets.ok()) {
      return targets;
    }
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.windows.push_back(std::move(window));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxPriorityTiers) {
    return make_error(ErrorCode::CorruptState, "policy tier count exceeds the bound");
  }
  fresh.tiers.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    PriorityTier tier;
    tier.index = reader.u32();
    tier.name = reader.str(limits::kMaxNameLength);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.tiers.push_back(std::move(tier));
  }
  count = reader.u32();
  if (!reader.ok() || count > kEvidenceKindCount) {
    return make_error(ErrorCode::CorruptState, "policy evidence-ttl count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t kind = reader.u32();
    const std::int64_t ttl = reader.i64();
    if (!reader.ok() || kind == 0 || kind > kEvidenceKindCount) {
      return make_error(ErrorCode::CorruptState, "policy record is malformed");
    }
    fresh.evidence_ttl[static_cast<EvidenceKind>(kind)] = ttl;
  }
  fresh.min_evidence_sources = reader.u32();
  fresh.require_window = reader.boolean();
  fresh.max_precondition_age = reader.i64();
  fresh.default_duration = reader.i64();
  fresh.max_concurrent_jobs = reader.u32();
  fresh.authority_lease = reader.i64();
  fresh.drain_lease = reader.i64();
  fresh.max_drain_failures_before_block = reader.u32();
  fresh.max_verification_attempts = reader.u32();
  fresh.max_restoration_attempts = reader.u32();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "policy record is malformed or has trailing bytes");
  }
  const Status valid = fresh.validate();
  if (!valid.ok()) {
    return make_error(ErrorCode::CorruptState, valid.error().detail);
  }
  policy = std::move(fresh);
  return ok_status();
}

Record encode_job_record(const MaintenanceJob& job) {
  ByteWriter writer;
  writer.u16(kJobLayout);
  writer.u64(job.id.value());
  writer.u64(job.generation.value());
  writer.u64(job.revision.value());
  writer.str(job.title, limits::kMaxTitleLength);
  writer.str(job.reason, limits::kMaxReasonLength);
  write_target_list(writer, job.targets);
  write_target_list(writer, job.removal_set);
  writer.u32(static_cast<std::uint32_t>(job.depends_on.size()));
  for (const JobId dependency : job.depends_on) {
    writer.u64(dependency.value());
  }
  writer.u32(static_cast<std::uint32_t>(job.window_filter.size()));
  for (const WindowId window : job.window_filter) {
    writer.u64(window.value());
  }
  writer.u32(job.priority);
  writer.i64(job.estimated_duration);
  writer.boolean(job.requires_drain);
  writer.u64(job.requestor.value());
  writer.u32(static_cast<std::uint32_t>(job.state));
  writer.u32(static_cast<std::uint32_t>(job.block));
  writer.str(job.block_detail, limits::kMaxDetailLength);
  writer.str(job.last_detail, limits::kMaxDetailLength);
  writer.u64(job.policy_revision.value());
  writer.u64(job.topology_revision.value());
  writer.u64(job.policy_digest);
  writer.u64(job.topology_digest);
  writer.u64(job.authority.value());
  writer.u64(job.lease.value());
  write_attempt_id(writer, job.active_attempt);
  writer.u64(job.next_attempt.value());
  writer.u32(static_cast<std::uint32_t>(job.attempts.size()));
  for (const AttemptRecord& attempt : job.attempts) {
    write_attempt_id(writer, attempt.id);
    writer.i64(attempt.started_at);
    writer.i64(attempt.ended_at);
    writer.u32(static_cast<std::uint32_t>(attempt.outcome));
    writer.str(attempt.detail, limits::kMaxDetailLength);
  }
  writer.u32(static_cast<std::uint32_t>(job.history.size()));
  for (const HistoryEntry& entry : job.history) {
    writer.i64(entry.at);
    writer.u32(static_cast<std::uint32_t>(entry.from));
    writer.u32(static_cast<std::uint32_t>(entry.to));
    writer.u32(static_cast<std::uint32_t>(entry.reason));
    writer.str(entry.detail, limits::kMaxDetailLength);
    write_incarnation(writer, entry.by);
  }
  writer.u32(job.history_dropped);
  writer.boolean(job.service_removed);
  writer.boolean(job.overrun);
  writer.boolean(job.approved);
  writer.str(job.approved_by, limits::kMaxNameLength);
  writer.i64(job.created_at);
  writer.i64(job.updated_at);
  writer.i64(job.started_at);
  writer.i64(job.completed_at);
  writer.i64(job.preconditions_at);
  writer.u64(job.preconditions_digest);
  writer.u64(job.preconditions_policy_revision.value());
  writer.u64(job.preconditions_topology_revision.value());
  writer.boolean(job.preconditions_satisfied);
  writer.u32(job.verification_passes);
  writer.u32(job.verification_failures);
  writer.u32(job.restoration_failures);
  writer.u32(job.drain_failures);
  writer.i64(job.verification_started_at);
  writer.i64(job.restoration_started_at);
  writer.boolean(job.maintenance_failed);
  writer.boolean(job.window_extended);
  writer.u64(job.window.value());
  writer.u32(static_cast<std::uint32_t>(job.on_close));
  writer.i64(job.window_closes_at);
  write_incarnation(writer, job.owner);
  Record out;
  out.type = RecordType::Job;
  out.payload = writer.take();
  return out;
}

Status decode_job_record(const Record& record, MaintenanceJob& job) {
  if (record.type != RecordType::Job) {
    return invalid_argument("record is not a job record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kJobLayout, "job");
  if (!layout.ok()) {
    return layout;
  }
  MaintenanceJob fresh;
  fresh.id = JobId::from_u64(reader.u64());
  fresh.generation = GenerationId::from_u64(reader.u64());
  fresh.revision = Revision::from_u64(reader.u64());
  fresh.title = reader.str(limits::kMaxTitleLength);
  fresh.reason = reader.str(limits::kMaxReasonLength);
  Status status = read_target_list(reader, fresh.targets, limits::kMaxTargetsPerJob);
  if (!status.ok()) {
    return status;
  }
  status = read_target_list(reader, fresh.removal_set, limits::kMaxTargetsPerTopology);
  if (!status.ok()) {
    return status;
  }
  std::uint32_t count = reader.u32();
  if (!reader.ok() || count > limits::kMaxDependenciesPerJob) {
    return make_error(ErrorCode::CorruptState, "job dependency count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    fresh.depends_on.push_back(JobId::from_u64(reader.u64()));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxWindowRules) {
    return make_error(ErrorCode::CorruptState, "job window-filter count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    fresh.window_filter.push_back(WindowId::from_u64(reader.u64()));
  }
  fresh.priority = reader.u32();
  fresh.estimated_duration = reader.i64();
  fresh.requires_drain = reader.boolean();
  fresh.requestor = RequestorId::from_u64(reader.u64());
  const std::uint32_t state = reader.u32();
  if (state >= kJobStateCount) {
    return make_error(ErrorCode::CorruptState, "job record carries an unknown lifecycle state");
  }
  fresh.state = static_cast<JobState>(state);
  const std::uint32_t block = reader.u32();
  if (block >= kBlockReasonCount) {
    return make_error(ErrorCode::CorruptState, "job record carries an unknown block reason");
  }
  fresh.block = static_cast<BlockReason>(block);
  fresh.block_detail = reader.str(limits::kMaxDetailLength);
  fresh.last_detail = reader.str(limits::kMaxDetailLength);
  fresh.policy_revision = Revision::from_u64(reader.u64());
  fresh.topology_revision = Revision::from_u64(reader.u64());
  fresh.policy_digest = reader.u64();
  fresh.topology_digest = reader.u64();
  fresh.authority = AuthorityId::from_u64(reader.u64());
  fresh.lease = DrainLeaseId::from_u64(reader.u64());
  fresh.active_attempt = read_attempt_id(reader);
  fresh.next_attempt = AttemptOrdinal::from_u64(reader.u64());
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxAttemptsPerJob) {
    return make_error(ErrorCode::CorruptState, "job attempt count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    AttemptRecord attempt;
    attempt.id = read_attempt_id(reader);
    attempt.started_at = reader.i64();
    attempt.ended_at = reader.i64();
    const std::uint32_t outcome = reader.u32();
    if (outcome > static_cast<std::uint32_t>(AttemptOutcome::Cancelled)) {
      return make_error(ErrorCode::CorruptState, "job record carries an unknown attempt outcome");
    }
    attempt.outcome = static_cast<AttemptOutcome>(outcome);
    attempt.detail = reader.str(limits::kMaxDetailLength);
    fresh.attempts.push_back(std::move(attempt));
  }
  count = reader.u32();
  if (!reader.ok() || count > limits::kMaxJobHistoryEntries) {
    return make_error(ErrorCode::CorruptState, "job history count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    HistoryEntry entry;
    entry.at = reader.i64();
    const std::uint32_t from = reader.u32();
    const std::uint32_t to = reader.u32();
    const std::uint32_t reason = reader.u32();
    if (from >= kJobStateCount || to >= kJobStateCount || reason >= kBlockReasonCount) {
      return make_error(ErrorCode::CorruptState, "job history entry is malformed");
    }
    entry.from = static_cast<JobState>(from);
    entry.to = static_cast<JobState>(to);
    entry.reason = static_cast<BlockReason>(reason);
    entry.detail = reader.str(limits::kMaxDetailLength);
    entry.by = read_incarnation(reader);
    fresh.history.push_back(std::move(entry));
  }
  fresh.history_dropped = reader.u32();
  fresh.service_removed = reader.boolean();
  fresh.overrun = reader.boolean();
  fresh.approved = reader.boolean();
  fresh.approved_by = reader.str(limits::kMaxNameLength);
  fresh.created_at = reader.i64();
  fresh.updated_at = reader.i64();
  fresh.started_at = reader.i64();
  fresh.completed_at = reader.i64();
  fresh.preconditions_at = reader.i64();
  fresh.preconditions_digest = reader.u64();
  fresh.preconditions_policy_revision = Revision::from_u64(reader.u64());
  fresh.preconditions_topology_revision = Revision::from_u64(reader.u64());
  fresh.preconditions_satisfied = reader.boolean();
  fresh.verification_passes = reader.u32();
  fresh.verification_failures = reader.u32();
  fresh.restoration_failures = reader.u32();
  fresh.drain_failures = reader.u32();
  fresh.verification_started_at = reader.i64();
  fresh.restoration_started_at = reader.i64();
  fresh.maintenance_failed = reader.boolean();
  fresh.window_extended = reader.boolean();
  fresh.window = WindowId::from_u64(reader.u64());
  const std::uint32_t on_close = reader.u32();
  if (on_close > static_cast<std::uint32_t>(OnWindowClose::AbortAndRestore)) {
    return make_error(ErrorCode::CorruptState, "job record carries an unknown window policy");
  }
  fresh.on_close = static_cast<OnWindowClose>(on_close);
  fresh.window_closes_at = reader.i64();
  fresh.owner = read_incarnation(reader);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "job record is malformed or has trailing bytes");
  }
  if (!fresh.id.valid() || !fresh.generation.valid()) {
    return make_error(ErrorCode::CorruptState, "job record has an invalid identity");
  }
  job = std::move(fresh);
  return ok_status();
}

Record encode_job_erased_record(JobId job) {
  ByteWriter writer;
  writer.u64(job.value());
  Record out;
  out.type = RecordType::JobErased;
  out.payload = writer.take();
  return out;
}

Status decode_job_erased_record(const Record& record, JobId& job) {
  if (record.type != RecordType::JobErased) {
    return invalid_argument("record is not a job-erased record");
  }
  ByteReader reader(record.payload);
  job = JobId::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end() || !job.valid()) {
    return make_error(ErrorCode::CorruptState, "job-erased record is malformed");
  }
  return ok_status();
}

Record encode_evidence_record(const EvidenceRecord& evidence) {
  ByteWriter writer;
  writer.u16(kEvidenceLayout);
  writer.u64(evidence.id.value());
  writer.u32(static_cast<std::uint32_t>(evidence.key.kind));
  writer.u32(static_cast<std::uint32_t>(evidence.key.subject.kind));
  write_target_id(writer, evidence.key.subject.target);
  write_domain_id(writer, evidence.key.subject.domain);
  writer.u64(evidence.key.subject.pool.value());
  writer.u64(evidence.key.subject.contract.value());
  writer.u64(evidence.key.source.value());
  writer.u64(evidence.revision.value());
  writer.i64(evidence.observed_at);
  writer.i64(evidence.ttl);
  writer.u32(static_cast<std::uint32_t>(evidence.klass));
  writer.u32(static_cast<std::uint32_t>(evidence.payload.health));
  writer.u32(evidence.payload.available_units);
  writer.u32(evidence.payload.total_units);
  writer.boolean(evidence.payload.result);
  writer.u32(evidence.payload.count);
  write_incarnation(writer, evidence.observed_by);
  writer.u64(evidence.job.value());
  write_attempt_id(writer, evidence.attempt);
  writer.u64(evidence.digest);
  Record out;
  out.type = RecordType::Evidence;
  out.payload = writer.take();
  return out;
}

Status decode_evidence_record(const Record& record, EvidenceRecord& evidence) {
  if (record.type != RecordType::Evidence) {
    return invalid_argument("record is not an evidence record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kEvidenceLayout, "evidence");
  if (!layout.ok()) {
    return layout;
  }
  EvidenceRecord fresh;
  fresh.id = EvidenceId::from_u64(reader.u64());
  const std::uint32_t kind = reader.u32();
  const std::uint32_t subject_kind = reader.u32();
  if (kind == 0 || kind > kEvidenceKindCount ||
      subject_kind > static_cast<std::uint32_t>(SubjectKind::Global)) {
    return make_error(ErrorCode::CorruptState, "evidence record has an unknown key kind");
  }
  fresh.key.kind = static_cast<EvidenceKind>(kind);
  fresh.key.subject.kind = static_cast<SubjectKind>(subject_kind);
  fresh.key.subject.target = read_target_id(reader);
  fresh.key.subject.domain = read_domain_id(reader);
  fresh.key.subject.pool = PoolId::from_u64(reader.u64());
  fresh.key.subject.contract = ContractId::from_u64(reader.u64());
  fresh.key.source = SourceId::from_u64(reader.u64());
  fresh.revision = Revision::from_u64(reader.u64());
  fresh.observed_at = reader.i64();
  fresh.ttl = reader.i64();
  const std::uint32_t klass = reader.u32();
  const std::uint32_t health = reader.u32();
  if (klass > static_cast<std::uint32_t>(EvidenceClass::Durable) ||
      health > static_cast<std::uint32_t>(Health::Retired)) {
    return make_error(ErrorCode::CorruptState, "evidence record has an unknown enumeration");
  }
  fresh.klass = static_cast<EvidenceClass>(klass);
  fresh.payload.health = static_cast<Health>(health);
  fresh.payload.available_units = reader.u32();
  fresh.payload.total_units = reader.u32();
  fresh.payload.result = reader.boolean();
  fresh.payload.count = reader.u32();
  fresh.observed_by = read_incarnation(reader);
  fresh.job = JobId::from_u64(reader.u64());
  fresh.attempt = read_attempt_id(reader);
  fresh.digest = reader.u64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "evidence record is malformed");
  }
  if (!fresh.valid()) {
    return make_error(ErrorCode::CorruptState, "evidence record is not well formed");
  }
  evidence = std::move(fresh);
  return ok_status();
}

Record encode_reservation_record(const AuthorityReservation& reservation) {
  ByteWriter writer;
  writer.u16(kReservationLayout);
  writer.u64(reservation.id.value());
  writer.u64(reservation.job.value());
  writer.u64(reservation.generation.value());
  write_attempt_id(writer, reservation.attempt);
  write_incarnation(writer, reservation.owner);
  write_target_list(writer, reservation.targets);
  writer.u32(static_cast<std::uint32_t>(reservation.domains.size()));
  for (const DomainId domain : reservation.domains) {
    write_domain_id(writer, domain);
  }
  writer.u32(static_cast<std::uint32_t>(reservation.pools.size()));
  for (const PoolId pool : reservation.pools) {
    writer.u64(pool.value());
  }
  writer.u64(reservation.revision.value());
  writer.i64(reservation.granted_at);
  writer.i64(reservation.expires_at);
  writer.u64(reservation.digest);
  Record out;
  out.type = RecordType::Reservation;
  out.payload = writer.take();
  return out;
}

Status decode_reservation_record(const Record& record, AuthorityReservation& reservation) {
  if (record.type != RecordType::Reservation) {
    return invalid_argument("record is not a reservation record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kReservationLayout, "reservation");
  if (!layout.ok()) {
    return layout;
  }
  AuthorityReservation fresh;
  fresh.id = AuthorityId::from_u64(reader.u64());
  fresh.job = JobId::from_u64(reader.u64());
  fresh.generation = GenerationId::from_u64(reader.u64());
  fresh.attempt = read_attempt_id(reader);
  fresh.owner = read_incarnation(reader);
  Status status = read_target_list(reader, fresh.targets, limits::kMaxTargetsPerTopology);
  if (!status.ok()) {
    return status;
  }
  const std::uint32_t domain_count = reader.u32();
  if (!reader.ok() || domain_count > limits::kMaxDomainsPerTopology) {
    return make_error(ErrorCode::CorruptState, "reservation domain count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    fresh.domains.push_back(read_domain_id(reader));
  }
  const std::uint32_t pool_count = reader.u32();
  if (!reader.ok() || pool_count > limits::kMaxPoolsPerTopology) {
    return make_error(ErrorCode::CorruptState, "reservation pool count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < pool_count; ++i) {
    fresh.pools.push_back(PoolId::from_u64(reader.u64()));
  }
  fresh.revision = Revision::from_u64(reader.u64());
  fresh.granted_at = reader.i64();
  fresh.expires_at = reader.i64();
  fresh.digest = reader.u64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "reservation record is malformed");
  }
  if (!fresh.id.valid() || !fresh.job.valid() || !fresh.attempt.valid() || !fresh.owner.valid()) {
    return make_error(ErrorCode::CorruptState, "reservation record is not well formed");
  }
  reservation = std::move(fresh);
  return ok_status();
}

Record encode_reservation_erased_record(AuthorityId id) {
  ByteWriter writer;
  writer.u64(id.value());
  Record out;
  out.type = RecordType::ReservationErased;
  out.payload = writer.take();
  return out;
}

Status decode_reservation_erased_record(const Record& record, AuthorityId& id) {
  if (record.type != RecordType::ReservationErased) {
    return invalid_argument("record is not a reservation-erased record");
  }
  ByteReader reader(record.payload);
  id = AuthorityId::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end() || !id.valid()) {
    return make_error(ErrorCode::CorruptState, "reservation-erased record is malformed");
  }
  return ok_status();
}

Record encode_lease_record(const DrainLease& lease) {
  ByteWriter writer;
  writer.u16(kLeaseLayout);
  writer.u64(lease.id.value());
  write_target_list(writer, lease.targets);
  writer.i64(lease.granted_at);
  writer.i64(lease.expires_at);
  writer.u32(static_cast<std::uint32_t>(lease.state));
  write_attempt_id(writer, lease.attempt);
  write_incarnation(writer, lease.owner);
  writer.u64(lease.epoch);
  Record out;
  out.type = RecordType::Lease;
  out.payload = writer.take();
  return out;
}

Status decode_lease_record(const Record& record, DrainLease& lease) {
  if (record.type != RecordType::Lease) {
    return invalid_argument("record is not a lease record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kLeaseLayout, "lease");
  if (!layout.ok()) {
    return layout;
  }
  DrainLease fresh;
  fresh.id = DrainLeaseId::from_u64(reader.u64());
  Status status = read_target_list(reader, fresh.targets, limits::kMaxDrainTargetsPerRequest);
  if (!status.ok()) {
    return status;
  }
  fresh.granted_at = reader.i64();
  fresh.expires_at = reader.i64();
  const std::uint32_t state = reader.u32();
  if (state > static_cast<std::uint32_t>(DrainState::NotFound)) {
    return make_error(ErrorCode::CorruptState, "lease record carries an unknown drain state");
  }
  fresh.state = static_cast<DrainState>(state);
  fresh.attempt = read_attempt_id(reader);
  fresh.owner = read_incarnation(reader);
  fresh.epoch = reader.u64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "lease record is malformed");
  }
  if (!fresh.id.valid()) {
    return make_error(ErrorCode::CorruptState, "lease record is not well formed");
  }
  lease = std::move(fresh);
  return ok_status();
}

Record encode_lease_erased_record(DrainLeaseId id) {
  ByteWriter writer;
  writer.u64(id.value());
  Record out;
  out.type = RecordType::LeaseErased;
  out.payload = writer.take();
  return out;
}

Status decode_lease_erased_record(const Record& record, DrainLeaseId& id) {
  if (record.type != RecordType::LeaseErased) {
    return invalid_argument("record is not a lease-erased record");
  }
  ByteReader reader(record.payload);
  id = DrainLeaseId::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end() || !id.valid()) {
    return make_error(ErrorCode::CorruptState, "lease-erased record is malformed");
  }
  return ok_status();
}

Record encode_quarantine_record(const QuarantineRecord& record) {
  ByteWriter writer;
  writer.u16(kQuarantineLayout);
  writer.u64(record.id.value());
  write_target_id(writer, record.target);
  writer.u64(record.job.value());
  writer.u64(record.generation.value());
  writer.u32(static_cast<std::uint32_t>(record.reason));
  writer.str(record.detail, limits::kMaxDetailLength);
  writer.i64(record.created_at);
  write_incarnation(writer, record.created_by);
  writer.boolean(record.cleared);
  writer.i64(record.cleared_at);
  writer.str(record.cleared_by, limits::kMaxNameLength);
  writer.u64(record.digest);
  Record out;
  out.type = RecordType::Quarantine;
  out.payload = writer.take();
  return out;
}

Status decode_quarantine_record(const Record& record, QuarantineRecord& out) {
  if (record.type != RecordType::Quarantine) {
    return invalid_argument("record is not a quarantine record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kQuarantineLayout, "quarantine");
  if (!layout.ok()) {
    return layout;
  }
  QuarantineRecord fresh;
  fresh.id = QuarantineId::from_u64(reader.u64());
  fresh.target = read_target_id(reader);
  fresh.job = JobId::from_u64(reader.u64());
  fresh.generation = GenerationId::from_u64(reader.u64());
  const std::uint32_t reason = reader.u32();
  if (reason >= kBlockReasonCount) {
    return make_error(ErrorCode::CorruptState, "quarantine record carries an unknown reason");
  }
  fresh.reason = static_cast<BlockReason>(reason);
  fresh.detail = reader.str(limits::kMaxDetailLength);
  fresh.created_at = reader.i64();
  fresh.created_by = read_incarnation(reader);
  fresh.cleared = reader.boolean();
  fresh.cleared_at = reader.i64();
  fresh.cleared_by = reader.str(limits::kMaxNameLength);
  fresh.digest = reader.u64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "quarantine record is malformed");
  }
  if (!fresh.id.valid() || !fresh.target.valid()) {
    return make_error(ErrorCode::CorruptState, "quarantine record is not well formed");
  }
  out = std::move(fresh);
  return ok_status();
}

Record encode_boot_record(const ControllerIncarnation& incarnation, BootEpoch prior_epoch) {
  ByteWriter writer;
  writer.u16(kBootLayout);
  write_incarnation(writer, incarnation);
  writer.u64(prior_epoch.value());
  Record out;
  out.type = RecordType::Boot;
  out.payload = writer.take();
  return out;
}

Status decode_boot_record(const Record& record, ControllerIncarnation& incarnation,
                          BootEpoch& prior_epoch) {
  if (record.type != RecordType::Boot) {
    return invalid_argument("record is not a boot record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kBootLayout, "boot");
  if (!layout.ok()) {
    return layout;
  }
  ControllerIncarnation fresh;
  fresh.controller = ControllerId::from_u64(reader.u64());
  fresh.boot_epoch = BootEpoch::from_u64(reader.u64());
  fresh.nonce = reader.u64();
  prior_epoch = BootEpoch::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "boot record is malformed");
  }
  if (!fresh.valid()) {
    return make_error(ErrorCode::CorruptState, "boot record is not well formed");
  }
  incarnation = fresh;
  return ok_status();
}

Record encode_marker_record(std::uint64_t value) {
  ByteWriter writer;
  writer.u64(value);
  Record out;
  out.type = RecordType::Marker;
  out.payload = writer.take();
  return out;
}

Record encode_transaction_record(const std::vector<Record>& records) {
  ByteWriter writer;
  writer.u16(kTransactionLayout);
  writer.u32(static_cast<std::uint32_t>(records.size()));
  for (const Record& record : records) {
    writer.u32(static_cast<std::uint32_t>(record.type));
    writer.blob(record.payload, limits::kMaxJournalRecordBytes);
  }
  Record out;
  out.type = RecordType::Transaction;
  out.payload = writer.take();
  return out;
}

Status decode_transaction_record(const Record& record, std::vector<Record>& out) {
  if (record.type != RecordType::Transaction) {
    return invalid_argument("record is not a transaction record");
  }
  ByteReader reader(record.payload);
  Status layout = check_layout(reader, kTransactionLayout, "transaction");
  if (!layout.ok()) {
    return layout;
  }
  const std::uint32_t count = reader.u32();
  if (!reader.ok() || count == 0 || count > limits::kMaxRecordsPerTransaction) {
    return make_error(ErrorCode::CorruptState, "transaction record count is out of range");
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t type = reader.u32();
    if (!is_known_record_type(static_cast<std::uint16_t>(type)) ||
        type == static_cast<std::uint32_t>(RecordType::Transaction)) {
      return make_error(ErrorCode::CorruptState, "transaction contains an unusable record type");
    }
    Record inner;
    inner.type = static_cast<RecordType>(type);
    const std::span<const std::byte> payload = reader.blob(limits::kMaxJournalRecordBytes);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "transaction record is malformed");
    }
    inner.payload.assign(payload.begin(), payload.end());
    out.push_back(std::move(inner));
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "transaction record has trailing bytes");
  }
  return ok_status();
}

}  // namespace mf
