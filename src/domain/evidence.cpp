// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/domain/evidence.hpp"

#include <algorithm>
#include <array>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

struct KindName {
  EvidenceKind kind;
  std::string_view name;
};

constexpr std::array<KindName, kEvidenceKindCount> kKindNames{{
    {EvidenceKind::TargetHealth, "target-health"},
    {EvidenceKind::PoolRedundancy, "pool-redundancy"},
    {EvidenceKind::ContractAvailability, "contract-availability"},
    {EvidenceKind::DrainLeaseState, "drain-lease-state"},
    {EvidenceKind::RestorationCheck, "restoration-check"},
    {EvidenceKind::MaintenanceCompletion, "maintenance-completion"},
    {EvidenceKind::ControlPlaneQuorum, "control-plane-quorum"},
}};

}  // namespace

const char* to_string(EvidenceKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.name == text) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

const char* to_string(SubjectKind kind) noexcept {
  switch (kind) {
    case SubjectKind::None: return "none";
    case SubjectKind::Target: return "target";
    case SubjectKind::Domain: return "domain";
    case SubjectKind::Pool: return "pool";
    case SubjectKind::Contract: return "contract";
    case SubjectKind::Global: return "global";
  }
  return "none";
}

const char* to_string(FreshnessState state) noexcept {
  switch (state) {
    case FreshnessState::Fresh: return "fresh";
    case FreshnessState::Missing: return "missing";
    case FreshnessState::Expired: return "expired";
    case FreshnessState::FutureDated: return "future-dated";
    case FreshnessState::ForeignIncarnation: return "foreign-incarnation";
    case FreshnessState::AttemptMismatch: return "attempt-mismatch";
    case FreshnessState::JobMismatch: return "job-mismatch";
  }
  return "missing";
}

EvidenceSubject EvidenceSubject::of(TargetId id) noexcept {
  EvidenceSubject subject;
  subject.kind = SubjectKind::Target;
  subject.target = id;
  return subject;
}

EvidenceSubject EvidenceSubject::of_domain(DomainId id) noexcept {
  EvidenceSubject subject;
  subject.kind = SubjectKind::Domain;
  subject.domain = id;
  return subject;
}

EvidenceSubject EvidenceSubject::of_pool(PoolId id) noexcept {
  EvidenceSubject subject;
  subject.kind = SubjectKind::Pool;
  subject.pool = id;
  return subject;
}

EvidenceSubject EvidenceSubject::of_contract(ContractId id) noexcept {
  EvidenceSubject subject;
  subject.kind = SubjectKind::Contract;
  subject.contract = id;
  return subject;
}

EvidenceSubject EvidenceSubject::global() noexcept {
  EvidenceSubject subject;
  subject.kind = SubjectKind::Global;
  return subject;
}

std::string to_string(const EvidenceSubject& subject) {
  switch (subject.kind) {
    case SubjectKind::Target: return to_string(subject.target);
    case SubjectKind::Domain: return to_string(subject.domain);
    case SubjectKind::Pool: return to_string(subject.pool);
    case SubjectKind::Contract: return to_string(subject.contract);
    case SubjectKind::Global: return "global";
    case SubjectKind::None: break;
  }
  return "none";
}

std::optional<EvidenceSubject> parse_evidence_subject(std::string_view text) {
  if (text == "global") {
    return EvidenceSubject::global();
  }
  if (const std::optional<TargetId> target = parse_target_id(text); target.has_value()) {
    return EvidenceSubject::of(*target);
  }
  if (const std::optional<DomainId> domain = parse_domain_id(text); domain.has_value()) {
    return EvidenceSubject::of_domain(*domain);
  }
  if (const std::optional<PoolId> pool = parse_id<PoolTag>(text); pool.has_value()) {
    return EvidenceSubject::of_pool(*pool);
  }
  if (const std::optional<ContractId> contract = parse_id<ContractTag>(text); contract.has_value()) {
    return EvidenceSubject::of_contract(*contract);
  }
  return std::nullopt;
}

std::string to_string(const EvidenceKey& key) {
  std::string out(to_string(key.kind));
  out.push_back('@');
  out += to_string(key.subject);
  out.push_back('#');
  out += to_string(key.source);
  return out;
}

bool EvidenceRecord::valid() const noexcept {
  return id.valid() && revision.valid() && key.source.valid() && key.subject.valid() &&
         observed_by.valid() && ttl > 0 && ttl <= limits::kMaxEvidenceTtlNanos;
}

EvidenceStore::EvidenceStore(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : std::min(capacity, limits::kMaxEvidenceRecords)) {}

Status EvidenceStore::observe(EvidenceRecord record) {
  if (!record.valid()) {
    ++rejections_;
    return invalid_argument("evidence record is not well formed");
  }
  if (record.digest == 0) {
    Digest digest;
    digest.update("mf.evidence.v1");
    digest.update_u32(static_cast<std::uint32_t>(record.key.kind));
    digest.update(to_string(record.key.subject));
    digest.update_u64(record.key.source.value());
    digest.update_u64(record.revision.value());
    digest.update_i64(record.observed_at);
    digest.update_i64(record.ttl);
    digest.update_u32(static_cast<std::uint32_t>(record.klass));
    digest.update_u32(static_cast<std::uint32_t>(record.payload.health));
    digest.update_u32(record.payload.available_units);
    digest.update_u32(record.payload.total_units);
    digest.update_bool(record.payload.result);
    digest.update_u32(record.payload.count);
    digest.update(to_string(record.observed_by));
    digest.update_u64(record.job.value());
    record.digest = digest.value();
  }

  const auto existing = records_.find(record.key);
  if (existing != records_.end()) {
    if (record.revision <= existing->second.revision) {
      ++rejections_;
      return make_error(ErrorCode::StaleRevision,
                        "evidence revision " + to_string(record.revision) +
                            " does not advance the stored revision " +
                            to_string(existing->second.revision) + " for " +
                            to_string(record.key));
    }
    if (record.observed_at < existing->second.observed_at) {
      ++rejections_;
      return make_error(ErrorCode::StaleEvidence,
                        "evidence observation time regressed for " + to_string(record.key));
    }
    existing->second = std::move(record);
    ++observations_;
    return ok_status();
  }

  if (records_.size() >= capacity_) {
    bool freed = false;
    for (auto it = records_.begin(); it != records_.end();) {
      if (it->second.observed_at + it->second.ttl <= record.observed_at) {
        it = records_.erase(it);
        ++evictions_;
        freed = true;
      } else {
        ++it;
      }
    }
    if (!freed) {
      ++rejections_;
      return make_error(ErrorCode::LimitExceeded,
                        "evidence store is full and holds no expired record to evict");
    }
  }
  records_.emplace(record.key, std::move(record));
  ++observations_;
  return ok_status();
}

const EvidenceRecord* EvidenceStore::find(const EvidenceKey& key) const noexcept {
  const auto it = records_.find(key);
  return it == records_.end() ? nullptr : &it->second;
}

Freshness EvidenceStore::evaluate(const EvidenceQuery& query) const {
  Freshness freshness;
  const EvidenceRecord* record = find(query.key);
  if (record == nullptr) {
    freshness.state = FreshnessState::Missing;
    freshness.detail = "no observation recorded for " + to_string(query.key);
    return freshness;
  }
  freshness.ttl = record->ttl;
  freshness.age = query.now - record->observed_at;

  if (query.job.valid() && record->job.valid() && record->job != query.job) {
    freshness.state = FreshnessState::JobMismatch;
    freshness.detail = "observation belongs to job " + to_string(record->job);
    return freshness;
  }
  if (query.attempt.valid() && record->attempt.valid() && record->attempt != query.attempt) {
    freshness.state = FreshnessState::AttemptMismatch;
    freshness.detail = "observation belongs to attempt " + to_string(record->attempt);
    return freshness;
  }
  if (record->observed_at - query.now > limits::kMaxClockSkewNanos) {
    freshness.state = FreshnessState::FutureDated;
    freshness.detail = "observation is dated in the future by " +
                       format_duration(record->observed_at - query.now);
    return freshness;
  }
  if (record->klass == EvidenceClass::Volatile) {
    if (is_fenced_by(record->observed_by, query.current)) {
      freshness.state = FreshnessState::ForeignIncarnation;
      freshness.detail = "observation was produced by incarnation " +
                         to_string(record->observed_by) +
                         " and must be re-observed by the running incarnation";
      return freshness;
    }
  }
  if (query.now > record->observed_at + record->ttl) {
    freshness.state = FreshnessState::Expired;
    freshness.detail = "observation is " + format_duration(freshness.age) + " old, ttl is " +
                       format_duration(record->ttl);
    return freshness;
  }
  freshness.state = FreshnessState::Fresh;
  freshness.detail = "fresh observation from " + to_string(query.key.source);
  return freshness;
}

std::vector<SourceId> EvidenceStore::fresh_sources(EvidenceKind kind,
                                                   const EvidenceSubject& subject,
                                                   const ControllerIncarnation& current,
                                                   Nanos now) const {
  std::vector<SourceId> out;
  for (const auto& [key, record] : records_) {
    if (key.kind != kind || key.subject != subject) {
      continue;
    }
    EvidenceQuery query;
    query.key = key;
    query.current = current;
    query.now = now;
    if (evaluate(query).fresh()) {
      out.push_back(key.source);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::size_t EvidenceStore::fresh_source_count(EvidenceKind kind, const EvidenceSubject& subject,
                                              const ControllerIncarnation& current,
                                              Nanos now) const {
  return fresh_sources(kind, subject, current, now).size();
}

std::vector<const EvidenceRecord*> EvidenceStore::records_for(EvidenceKind kind,
                                                              const EvidenceSubject& subject) const {
  std::vector<const EvidenceRecord*> out;
  for (const auto& [key, record] : records_) {
    if (key.kind == kind && key.subject == subject) {
      out.push_back(&record);
    }
  }
  return out;
}

bool EvidenceStore::evict_expired(Nanos now) {
  bool freed = false;
  for (auto it = records_.begin(); it != records_.end();) {
    if (it->second.observed_at + it->second.ttl <= now) {
      it = records_.erase(it);
      ++evictions_;
      freed = true;
    } else {
      ++it;
    }
  }
  return freed;
}

void EvidenceStore::clear() {
  records_.clear();
  observations_ = 0;
  rejections_ = 0;
  evictions_ = 0;
}

}  // namespace mf
