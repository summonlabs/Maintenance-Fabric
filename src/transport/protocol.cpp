// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/transport/protocol.hpp"

#include <algorithm>
#include <array>
#include <tuple>

#include "mf/core/bytes.hpp"
#include "mf/core/limits.hpp"

namespace mf {
namespace {

constexpr std::uint16_t kEnvelopeLayout = 1;
constexpr std::uint16_t kBodyLayout = 1;

struct OpName {
  OpCode op;
  std::string_view name;
};

constexpr std::array<OpName, 21> kOpNames{{
    {OpCode::Ping, "ping"},
    {OpCode::Status, "status"},
    {OpCode::List, "list"},
    {OpCode::Propose, "propose"},
    {OpCode::Approve, "approve"},
    {OpCode::Rearm, "rearm"},
    {OpCode::Cancel, "cancel"},
    {OpCode::Tick, "tick"},
    {OpCode::Explain, "explain"},
    {OpCode::Conflicts, "conflicts"},
    {OpCode::ReportCompletion, "report-completion"},
    {OpCode::ReportRestoration, "report-restoration"},
    {OpCode::Observe, "observe"},
    {OpCode::Quarantines, "quarantines"},
    {OpCode::ClearQuarantine, "clear-quarantine"},
    {OpCode::DrainRequest, "drain-request"},
    {OpCode::DrainRelease, "drain-release"},
    {OpCode::DrainQuery, "drain-query"},
    {OpCode::DrainRefresh, "drain-refresh"},
    {OpCode::Ping, "reserved"},
    {OpCode::Ping, "reserved-2"},
}};

void write_target(ByteWriter& writer, TargetId id) {
  writer.u32(static_cast<std::uint32_t>(id.kind));
  writer.u32(id.index);
}

TargetId read_target(ByteReader& reader) {
  const std::uint32_t kind = reader.u32();
  const std::uint32_t index = reader.u32();
  TargetId id;
  id.kind = kind < kTargetKindCount ? static_cast<TargetKind>(kind) : TargetKind::Unknown;
  id.index = index;
  return id;
}

void write_incarnation(ByteWriter& writer, const ControllerIncarnation& value) {
  writer.u64(value.controller.value());
  writer.u64(value.boot_epoch.value());
  writer.u64(value.nonce);
}

ControllerIncarnation read_incarnation(ByteReader& reader) {
  ControllerIncarnation value;
  value.controller = ControllerId::from_u64(reader.u64());
  value.boot_epoch = BootEpoch::from_u64(reader.u64());
  value.nonce = reader.u64();
  return value;
}

void write_attempt(ByteWriter& writer, const AttemptId& id) {
  writer.u64(id.job.value());
  writer.u64(id.generation.value());
  writer.u64(id.ordinal.value());
}

AttemptId read_attempt(ByteReader& reader) {
  AttemptId id;
  id.job = JobId::from_u64(reader.u64());
  id.generation = GenerationId::from_u64(reader.u64());
  id.ordinal = AttemptOrdinal::from_u64(reader.u64());
  return id;
}

}  // namespace

const char* to_string(OpCode op) noexcept {
  for (const OpName& entry : kOpNames) {
    if (entry.op == op) {
      return entry.name.data();
    }
  }
  return "unknown";
}

bool is_known_op(std::uint16_t value) noexcept {
  switch (static_cast<OpCode>(value)) {
    case OpCode::Ping:
    case OpCode::Status:
    case OpCode::List:
    case OpCode::Propose:
    case OpCode::Approve:
    case OpCode::Rearm:
    case OpCode::Cancel:
    case OpCode::Tick:
    case OpCode::Explain:
    case OpCode::Conflicts:
    case OpCode::ReportCompletion:
    case OpCode::ReportRestoration:
    case OpCode::Observe:
    case OpCode::Quarantines:
    case OpCode::ClearQuarantine:
    case OpCode::DrainRequest:
    case OpCode::DrainRelease:
    case OpCode::DrainQuery:
    case OpCode::DrainRefresh:
      return true;
  }
  return false;
}

std::vector<std::byte> encode_envelope(const RpcEnvelope& envelope) {
  ByteWriter writer;
  writer.u16(kEnvelopeLayout);
  writer.u16(envelope.op);
  writer.str(envelope.message, limits::kMaxDetailLength);
  writer.blob(envelope.body, limits::kMaxFrameBytes);
  return writer.take();
}

Result<RpcEnvelope> decode_envelope(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  const std::uint16_t layout = reader.u16();
  if (!reader.ok()) {
    return make_error(ErrorCode::CorruptState, "rpc envelope is truncated");
  }
  if (layout != kEnvelopeLayout) {
    return make_error(ErrorCode::VersionMismatch,
                      "rpc envelope layout " + std::to_string(layout) + " is not supported");
  }
  RpcEnvelope envelope;
  envelope.op = reader.u16();
  envelope.message = reader.str(limits::kMaxDetailLength);
  const std::span<const std::byte> body = reader.blob(limits::kMaxFrameBytes);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "rpc envelope is malformed");
  }
  envelope.body.assign(body.begin(), body.end());
  return envelope;
}

std::vector<std::byte> encode_actor(std::string_view actor, std::string_view reason) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.str(actor, limits::kMaxNameLength);
  writer.str(reason, limits::kMaxReasonLength);
  return writer.take();
}

Result<std::pair<std::string, std::string>> decode_actor(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "actor body layout is not supported");
  }
  std::string actor = reader.str(limits::kMaxNameLength);
  std::string reason = reader.str(limits::kMaxReasonLength);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "actor body is malformed");
  }
  return std::make_pair(std::move(actor), std::move(reason));
}

std::vector<std::byte> encode_request_body(const MaintenanceRequest& request) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.str(request.title, limits::kMaxTitleLength);
  writer.str(request.reason, limits::kMaxReasonLength);
  writer.u32(static_cast<std::uint32_t>(request.targets.size()));
  for (const TargetId target : request.targets) {
    write_target(writer, target);
  }
  writer.u32(static_cast<std::uint32_t>(request.depends_on.size()));
  for (const JobId dependency : request.depends_on) {
    writer.u64(dependency.value());
  }
  writer.u32(static_cast<std::uint32_t>(request.window_filter.size()));
  for (const WindowId window : request.window_filter) {
    writer.u64(window.value());
  }
  writer.u32(request.priority);
  writer.i64(request.estimated_duration);
  writer.boolean(request.requires_drain);
  writer.u64(request.requestor.value());
  return writer.take();
}

Result<MaintenanceRequest> decode_request_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "request body layout is not supported");
  }
  MaintenanceRequest request;
  request.title = reader.str(limits::kMaxTitleLength);
  request.reason = reader.str(limits::kMaxReasonLength);
  const std::uint32_t target_count = reader.u32();
  if (!reader.ok() || target_count > limits::kMaxTargetsPerJob) {
    return make_error(ErrorCode::LimitExceeded, "request body declares too many targets");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    const TargetId target = read_target(reader);
    if (!target.valid()) {
      return make_error(ErrorCode::CorruptState, "request body carries an invalid target");
    }
    request.targets.push_back(target);
  }
  const std::uint32_t dependency_count = reader.u32();
  if (!reader.ok() || dependency_count > limits::kMaxDependenciesPerJob) {
    return make_error(ErrorCode::LimitExceeded, "request body declares too many dependencies");
  }
  for (std::uint32_t i = 0; i < dependency_count; ++i) {
    request.depends_on.push_back(JobId::from_u64(reader.u64()));
  }
  const std::uint32_t window_count = reader.u32();
  if (!reader.ok() || window_count > limits::kMaxWindowRules) {
    return make_error(ErrorCode::LimitExceeded, "request body declares too many windows");
  }
  for (std::uint32_t i = 0; i < window_count; ++i) {
    request.window_filter.push_back(WindowId::from_u64(reader.u64()));
  }
  request.priority = reader.u32();
  request.estimated_duration = reader.i64();
  request.requires_drain = reader.boolean();
  request.requestor = RequestorId::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "request body is malformed");
  }
  return request;
}

std::vector<std::byte> encode_job_ref(JobId job, std::string_view text) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(job.value());
  writer.str(text, limits::kMaxReasonLength);
  return writer.take();
}

Result<std::pair<JobId, std::string>> decode_job_ref(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "job reference layout is not supported");
  }
  const JobId job = JobId::from_u64(reader.u64());
  std::string text = reader.str(limits::kMaxReasonLength);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "job reference is malformed");
  }
  return std::make_pair(job, std::move(text));
}

std::vector<std::byte> encode_snapshot(const JobSnapshot& snapshot) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(snapshot.id.value());
  writer.u64(snapshot.generation.value());
  writer.u64(snapshot.revision.value());
  writer.u32(static_cast<std::uint32_t>(snapshot.state));
  writer.u32(static_cast<std::uint32_t>(snapshot.block));
  writer.str(snapshot.block_detail, limits::kMaxDetailLength);
  writer.u32(static_cast<std::uint32_t>(snapshot.targets.size()));
  for (const TargetId target : snapshot.targets) {
    write_target(writer, target);
  }
  writer.u32(snapshot.priority);
  writer.boolean(snapshot.service_removed);
  writer.boolean(snapshot.overrun);
  writer.u64(snapshot.authority.value());
  writer.u64(snapshot.lease.value());
  write_attempt(writer, snapshot.active_attempt);
  writer.i64(snapshot.created_at);
  writer.i64(snapshot.updated_at);
  return writer.take();
}

Result<JobSnapshot> decode_snapshot(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "snapshot layout is not supported");
  }
  JobSnapshot snapshot;
  snapshot.id = JobId::from_u64(reader.u64());
  snapshot.generation = GenerationId::from_u64(reader.u64());
  snapshot.revision = Revision::from_u64(reader.u64());
  const std::uint32_t state = reader.u32();
  const std::uint32_t block = reader.u32();
  if (state >= kJobStateCount || block >= kBlockReasonCount) {
    return make_error(ErrorCode::CorruptState, "snapshot carries an unknown enumeration");
  }
  snapshot.state = static_cast<JobState>(state);
  snapshot.block = static_cast<BlockReason>(block);
  snapshot.block_detail = reader.str(limits::kMaxDetailLength);
  const std::uint32_t target_count = reader.u32();
  if (!reader.ok() || target_count > limits::kMaxTargetsPerJob) {
    return make_error(ErrorCode::LimitExceeded, "snapshot declares too many targets");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    snapshot.targets.push_back(read_target(reader));
  }
  snapshot.priority = reader.u32();
  snapshot.service_removed = reader.boolean();
  snapshot.overrun = reader.boolean();
  snapshot.authority = AuthorityId::from_u64(reader.u64());
  snapshot.lease = DrainLeaseId::from_u64(reader.u64());
  snapshot.active_attempt = read_attempt(reader);
  snapshot.created_at = reader.i64();
  snapshot.updated_at = reader.i64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "snapshot is malformed");
  }
  return snapshot;
}

std::vector<std::byte> encode_snapshot_list(const std::vector<JobSnapshot>& snapshots) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  const std::uint32_t count = static_cast<std::uint32_t>(
      std::min<std::size_t>(snapshots.size(), limits::kMaxJobsPerArbitration));
  writer.u32(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::vector<std::byte> encoded = encode_snapshot(snapshots[i]);
    writer.blob(encoded, limits::kMaxFrameBytes / 2);
  }
  return writer.take();
}

Result<std::vector<JobSnapshot>> decode_snapshot_list(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "snapshot list layout is not supported");
  }
  const std::uint32_t count = reader.u32();
  if (!reader.ok() || count > limits::kMaxJobsPerArbitration) {
    return make_error(ErrorCode::LimitExceeded, "snapshot list declares too many entries");
  }
  std::vector<JobSnapshot> out;
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::span<const std::byte> blob = reader.blob(limits::kMaxFrameBytes / 2);
    if (!reader.ok()) {
      return make_error(ErrorCode::CorruptState, "snapshot list is malformed");
    }
    Result<JobSnapshot> snapshot = decode_snapshot(blob);
    if (!snapshot.ok()) {
      return snapshot.error();
    }
    out.push_back(std::move(snapshot.value()));
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "snapshot list has trailing bytes");
  }
  return out;
}

std::vector<std::byte> encode_job_actor(JobId job, std::string_view actor,
                                          std::string_view reason) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(job.value());
  writer.str(actor, limits::kMaxNameLength);
  writer.str(reason, limits::kMaxReasonLength);
  return writer.take();
}

Result<std::tuple<JobId, std::string, std::string>> decode_job_actor(
    std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "job-actor body layout is not supported");
  }
  const JobId job = JobId::from_u64(reader.u64());
  std::string actor = reader.str(limits::kMaxNameLength);
  std::string reason = reader.str(limits::kMaxReasonLength);
  if (!reader.ok() || !reader.at_end() || !job.valid()) {
    return make_error(ErrorCode::CorruptState, "job-actor body is malformed");
  }
  return std::make_tuple(job, std::move(actor), std::move(reason));
}

std::vector<std::byte> encode_quarantine_clear(QuarantineId id, std::string_view actor,
                                               std::string_view justification) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(id.value());
  writer.str(actor, limits::kMaxNameLength);
  writer.str(justification, limits::kMaxReasonLength);
  return writer.take();
}

Result<std::tuple<QuarantineId, std::string, std::string>> decode_quarantine_clear(
    std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "quarantine-clear body layout is not supported");
  }
  const QuarantineId id = QuarantineId::from_u64(reader.u64());
  std::string actor = reader.str(limits::kMaxNameLength);
  std::string justification = reader.str(limits::kMaxReasonLength);
  if (!reader.ok() || !reader.at_end() || !id.valid()) {
    return make_error(ErrorCode::CorruptState, "quarantine-clear body is malformed");
  }
  return std::make_tuple(id, std::move(actor), std::move(justification));
}

std::vector<std::byte> encode_completion_report(JobId job, const AttemptId& attempt, bool succeeded,
                                                std::string_view detail) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(job.value());
  write_attempt(writer, attempt);
  writer.boolean(succeeded);
  writer.str(detail, limits::kMaxDetailLength);
  return writer.take();
}

Result<std::tuple<JobId, AttemptId, bool, std::string>> decode_completion_report(
    std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "completion report layout is not supported");
  }
  const JobId job = JobId::from_u64(reader.u64());
  const AttemptId attempt = read_attempt(reader);
  const bool succeeded = reader.boolean();
  std::string detail = reader.str(limits::kMaxDetailLength);
  if (!reader.ok() || !reader.at_end() || !attempt.valid() || !job.valid()) {
    return make_error(ErrorCode::CorruptState, "completion report is malformed");
  }
  return std::make_tuple(job, attempt, succeeded, std::move(detail));
}

std::vector<std::byte> encode_restoration_report(JobId job, const AttemptId& attempt,
                                                 TargetId target, bool ok,
                                                 std::string_view detail) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(job.value());
  write_attempt(writer, attempt);
  write_target(writer, target);
  writer.boolean(ok);
  writer.str(detail, limits::kMaxDetailLength);
  return writer.take();
}

Result<std::tuple<JobId, AttemptId, TargetId, bool, std::string>> decode_restoration_report(
    std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "restoration report layout is not supported");
  }
  const JobId job = JobId::from_u64(reader.u64());
  const AttemptId attempt = read_attempt(reader);
  const TargetId target = read_target(reader);
  const bool ok = reader.boolean();
  std::string detail = reader.str(limits::kMaxDetailLength);
  if (!reader.ok() || !reader.at_end() || !attempt.valid() || !target.valid() || !job.valid()) {
    return make_error(ErrorCode::CorruptState, "restoration report is malformed");
  }
  return std::make_tuple(job, attempt, target, ok, std::move(detail));
}

std::vector<std::byte> encode_evidence_body(const EvidenceRecord& evidence) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u32(static_cast<std::uint32_t>(evidence.key.kind));
  writer.u32(static_cast<std::uint32_t>(evidence.key.subject.kind));
  write_target(writer, evidence.key.subject.target);
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
  writer.u64(evidence.job.value());
  return writer.take();
}

Result<EvidenceRecord> decode_evidence_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "evidence body layout is not supported");
  }
  EvidenceRecord evidence;
  const std::uint32_t kind = reader.u32();
  const std::uint32_t subject_kind = reader.u32();
  if (kind == 0 || kind > kEvidenceKindCount ||
      subject_kind > static_cast<std::uint32_t>(SubjectKind::Global)) {
    return make_error(ErrorCode::CorruptState, "evidence body carries an unknown key");
  }
  evidence.key.kind = static_cast<EvidenceKind>(kind);
  evidence.key.subject.kind = static_cast<SubjectKind>(subject_kind);
  evidence.key.subject.target = read_target(reader);
  evidence.key.source = SourceId::from_u64(reader.u64());
  evidence.revision = Revision::from_u64(reader.u64());
  evidence.observed_at = reader.i64();
  evidence.ttl = reader.i64();
  const std::uint32_t klass = reader.u32();
  const std::uint32_t health = reader.u32();
  if (klass > static_cast<std::uint32_t>(EvidenceClass::Durable) ||
      health > static_cast<std::uint32_t>(Health::Retired)) {
    return make_error(ErrorCode::CorruptState, "evidence body carries an unknown enumeration");
  }
  evidence.klass = static_cast<EvidenceClass>(klass);
  evidence.payload.health = static_cast<Health>(health);
  evidence.payload.available_units = reader.u32();
  evidence.payload.total_units = reader.u32();
  evidence.payload.result = reader.boolean();
  evidence.payload.count = reader.u32();
  evidence.job = JobId::from_u64(reader.u64());
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "evidence body is malformed");
  }
  return evidence;
}

std::vector<std::byte> encode_drain_request_body(const DrainRequest& request) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(request.job.value());
  writer.u64(request.generation.value());
  write_attempt(writer, request.attempt);
  writer.u32(static_cast<std::uint32_t>(request.targets.size()));
  for (const TargetId target : request.targets) {
    write_target(writer, target);
  }
  writer.i64(request.lease_duration);
  writer.str(request.reason, limits::kMaxReasonLength);
  write_incarnation(writer, request.requester);
  return writer.take();
}

Result<DrainRequest> decode_drain_request_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "drain request layout is not supported");
  }
  DrainRequest request;
  request.job = JobId::from_u64(reader.u64());
  request.generation = GenerationId::from_u64(reader.u64());
  request.attempt = read_attempt(reader);
  const std::uint32_t target_count = reader.u32();
  if (!reader.ok() || target_count > limits::kMaxDrainTargetsPerRequest) {
    return make_error(ErrorCode::LimitExceeded, "drain request declares too many targets");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    request.targets.push_back(read_target(reader));
  }
  request.lease_duration = reader.i64();
  request.reason = reader.str(limits::kMaxReasonLength);
  request.requester = read_incarnation(reader);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "drain request is malformed");
  }
  return request;
}

std::vector<std::byte> encode_drain_lease_body(const DrainLease& lease) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(lease.id.value());
  writer.u32(static_cast<std::uint32_t>(lease.targets.size()));
  for (const TargetId target : lease.targets) {
    write_target(writer, target);
  }
  writer.i64(lease.granted_at);
  writer.i64(lease.expires_at);
  writer.u32(static_cast<std::uint32_t>(lease.state));
  write_attempt(writer, lease.attempt);
  write_incarnation(writer, lease.owner);
  writer.u64(lease.epoch);
  return writer.take();
}

Result<DrainLease> decode_drain_lease_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "drain lease layout is not supported");
  }
  DrainLease lease;
  lease.id = DrainLeaseId::from_u64(reader.u64());
  const std::uint32_t target_count = reader.u32();
  if (!reader.ok() || target_count > limits::kMaxDrainTargetsPerRequest) {
    return make_error(ErrorCode::LimitExceeded, "drain lease declares too many targets");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    lease.targets.push_back(read_target(reader));
  }
  lease.granted_at = reader.i64();
  lease.expires_at = reader.i64();
  const std::uint32_t state = reader.u32();
  if (state > static_cast<std::uint32_t>(DrainState::NotFound)) {
    return make_error(ErrorCode::CorruptState, "drain lease carries an unknown state");
  }
  lease.state = static_cast<DrainState>(state);
  lease.attempt = read_attempt(reader);
  lease.owner = read_incarnation(reader);
  lease.epoch = reader.u64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "drain lease is malformed");
  }
  return lease;
}

std::vector<std::byte> encode_drain_lease_ref(DrainLeaseId lease,
                                              const ControllerIncarnation& requester,
                                              Nanos extend) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.u64(lease.value());
  write_incarnation(writer, requester);
  writer.i64(extend);
  return writer.take();
}

Result<std::tuple<DrainLeaseId, ControllerIncarnation, Nanos>> decode_drain_lease_ref(
    std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "drain lease reference layout is not supported");
  }
  const DrainLeaseId lease = DrainLeaseId::from_u64(reader.u64());
  const ControllerIncarnation requester = read_incarnation(reader);
  const Nanos extend = reader.i64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "drain lease reference is malformed");
  }
  return std::make_tuple(lease, requester, extend);
}

std::vector<std::byte> encode_text_body(std::string_view text) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.blob(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                         text.size()),
              limits::kMaxFrameBytes / 2);
  return writer.take();
}

Result<std::string> decode_text_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "text body layout is not supported");
  }
  const std::span<const std::byte> blob = reader.blob(limits::kMaxFrameBytes / 2);
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "text body is malformed");
  }
  return std::string(reinterpret_cast<const char*>(blob.data()), blob.size());
}

std::vector<std::byte> encode_i64_body(std::int64_t value) {
  ByteWriter writer;
  writer.u16(kBodyLayout);
  writer.i64(value);
  return writer.take();
}

Result<std::int64_t> decode_i64_body(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  if (reader.u16() != kBodyLayout) {
    return make_error(ErrorCode::CorruptState, "integer body layout is not supported");
  }
  const std::int64_t value = reader.i64();
  if (!reader.ok() || !reader.at_end()) {
    return make_error(ErrorCode::CorruptState, "integer body is malformed");
  }
  return value;
}

}  // namespace mf
