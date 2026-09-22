// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/engine/explain.hpp"

#include <algorithm>

#include "mf/core/hash.hpp"

namespace mf {
namespace {

void line(std::vector<ExplanationLine>& lines, std::string code, std::string text) {
  lines.push_back(ExplanationLine{std::move(code), std::move(text)});
}

}  // namespace

std::string Explanation::to_text() const {
  std::string out;
  out += "subject: ";
  out += subject;
  out += "\naction: ";
  out += action;
  out += "\noutcome: ";
  out += outcome;
  out += "\nat: ";
  out += format_time(at);
  out += "\nby: ";
  out += to_string(by);
  out += "\npolicy: ";
  out += to_string(policy_revision);
  out += " digest=";
  out += hex_u64(policy_digest);
  out += "\ntopology: ";
  out += to_string(topology_revision);
  out += " digest=";
  out += hex_u64(topology_digest);
  out += "\nreadiness digest=";
  out += hex_u64(readiness_digest);
  out += " impact digest=";
  out += hex_u64(impact_digest);
  out += " preconditions digest=";
  out += hex_u64(preconditions_digest);
  out += "\nexplanation digest=";
  out += hex_u64(digest);
  out.push_back('\n');

  const auto dump = [&out](const char* heading, const std::vector<ExplanationLine>& lines) {
    out += heading;
    out += ":\n";
    if (lines.empty()) {
      out += "  (none)\n";
      return;
    }
    for (const ExplanationLine& entry : lines) {
      out += "  [";
      out += entry.code;
      out += "] ";
      out += entry.text;
      out.push_back('\n');
    }
  };
  dump("evidence", evidence);
  dump("policy", policy);
  dump("lifecycle", lifecycle);
  dump("rejected alternatives", rejected);
  return out;
}

Explanation explain_job(const ExplainContext& context) {
  Explanation explanation;
  explanation.at = context.now;
  explanation.by = context.current;
  explanation.action = context.action;
  if (context.policy != nullptr) {
    explanation.policy_revision = context.policy->revision;
    explanation.policy_digest = context.policy->digest();
  }
  if (context.topology != nullptr) {
    explanation.topology_revision = context.topology->revision();
    explanation.topology_digest = context.topology->digest();
  }
  if (context.readiness != nullptr) {
    explanation.readiness_digest = context.readiness->digest();
  }
  if (context.impact != nullptr) {
    explanation.impact_digest = context.impact->digest;
  }
  if (context.preconditions != nullptr) {
    explanation.preconditions_digest = context.preconditions->digest;
  }

  const MaintenanceJob* job = context.job;
  if (job == nullptr) {
    explanation.subject = "unknown";
    explanation.outcome = "no such job";
    return explanation;
  }
  explanation.subject = to_string(job->id) + " generation " + to_string(job->generation) +
                        " revision " + to_string(job->revision);
  explanation.job = job->snapshot();
  explanation.outcome = std::string(to_string(job->state));
  if (job->block != BlockReason::None) {
    explanation.outcome += " blocked by ";
    explanation.outcome += to_string(job->block);
  }

  line(explanation.lifecycle, "state", std::string(to_string(job->state)) + " -- " +
                                          describe(job->state));
  line(explanation.lifecycle, "service-removed",
       job->service_removed ? "true (backward transitions and cancellation are forbidden)"
                            : "false");
  line(explanation.lifecycle, "generation",
       "job " + to_string(job->id) + " generation " + to_string(job->generation) + ", attempt " +
           to_string(job->active_attempt));
  {
    std::string allowed;
    for (const JobState next : allowed_next_states(job->state, job->service_removed)) {
      if (!allowed.empty()) {
        allowed += ",";
      }
      allowed += to_string(next);
    }
    line(explanation.lifecycle, "allowed-next", allowed.empty() ? "(terminal)" : allowed);
  }
  for (const HistoryEntry& entry : job->history) {
    line(explanation.lifecycle, to_string(entry.reason),
         format_time(entry.at) + " " + to_string(entry.from) + " -> " + to_string(entry.to) +
             (entry.detail.empty() ? std::string() : " -- " + entry.detail));
  }

  if (context.readiness != nullptr) {
    for (const TargetId target : job->removal_set) {
      const TargetReadiness* readiness = context.readiness->get(target);
      if (readiness == nullptr) {
        line(explanation.evidence, "readiness-absent",
             to_string(target) + " is not present in the readiness view");
        continue;
      }
      line(explanation.evidence, readiness->fresh ? "readiness-fresh" : "readiness-stale",
           to_string(target) + ": health=" + to_string(readiness->health) + " sources=" +
               std::to_string(readiness->fresh_sources) + " state=" +
               to_string(readiness->state) + " -- " + readiness->detail);
    }
  }
  if (context.ledger != nullptr) {
    const AuthorityReservation* reservation =
        context.ledger->find_for_job(job->id, job->generation);
    if (reservation == nullptr) {
      line(explanation.evidence, "authority-absent",
           "job holds no maintenance authority in the current incarnation");
    } else {
      line(explanation.evidence, "authority-held",
           reservation->to_text() + " attempt=" + to_string(reservation->attempt));
    }
  }

  if (context.policy != nullptr) {
    const Policy& policy = *context.policy;
    line(explanation.policy, "tier",
         "priority tier " + std::to_string(job->priority) + " rank " +
             std::to_string(policy.tier_rank(job->priority)) + " of " +
             std::to_string(policy.tiers.size()));
    line(explanation.policy, "window",
         policy.require_window ? "a maintenance window is required"
                               : "a maintenance window is optional");
    line(explanation.policy, "evidence-sources",
         "at least " + std::to_string(policy.min_evidence_sources) +
             " fresh source(s) required per target");
    line(explanation.policy, "concurrency",
         "policy admits at most " + std::to_string(policy.max_concurrent_jobs) +
             " concurrent maintenance job(s)");
    for (const PoolId pool : context.topology != nullptr
                                 ? context.topology->pools_of(job->removal_set)
                                 : std::vector<PoolId>{}) {
      const RedundancyRule rule = policy.redundancy_for(pool);
      line(explanation.policy, "redundancy",
           "pool " + to_string(pool) + " requires " + std::to_string(rule.required_serving_units()) +
               " serving unit(s) (N=" + std::to_string(rule.min_viable) + " +" +
               std::to_string(rule.tolerated_losses) + ") via rule '" + rule.name + "'");
    }
  }

  if (context.impact != nullptr) {
    for (const PoolImpact& pool : context.impact->pools) {
      line(explanation.rejected, pool.satisfied ? "pool-ok" : "pool-violation",
           "pool " + to_string(pool.pool) + " total=" + std::to_string(pool.total_units) +
               " serving=" + std::to_string(pool.serving_units) + " removed=" +
               std::to_string(pool.removed_serving_units) + " remaining=" +
               std::to_string(pool.remaining_units) + " required=" +
               std::to_string(pool.required_units + pool.reserve_units) +
               (pool.reason.empty() ? std::string() : " -- " + pool.reason));
    }
    for (const DomainImpact& domain : context.impact->domains) {
      line(explanation.rejected, domain.satisfied ? "domain-ok" : "domain-violation",
           "domain " + to_string(domain.domain) + " out_before=" +
               std::to_string(domain.out_before) + " out_after=" + std::to_string(domain.out_after) +
               (domain.limited ? " max_out=" + std::to_string(domain.max_out) : " unlimited") +
               (domain.reason.empty() ? std::string() : " -- " + domain.reason));
    }
    for (const ContractImpact& contract : context.impact->contracts) {
      line(explanation.rejected, contract.satisfied ? "contract-ok" : "contract-violation",
           "contract " + to_string(contract.contract) + " available_before=" +
               std::to_string(contract.available_before) + " available_after=" +
               std::to_string(contract.available_after) + " min=" +
               std::to_string(contract.min_available) +
               (contract.reason.empty() ? std::string() : " -- " + contract.reason));
    }
  }
  if (context.preconditions != nullptr) {
    for (const PreconditionResult& result : context.preconditions->results) {
      if (result.status == PreconditionStatus::Satisfied) {
        continue;
      }
      line(explanation.rejected, std::string("precondition-") + to_string(result.status) + "-" +
                                     to_string(result.kind),
           result.detail);
    }
  }
  if (context.jobs != nullptr && context.ledger != nullptr) {
    for (const auto& [id, reservation] : context.ledger->reservations()) {
      if (reservation.job == job->id) {
        continue;
      }
      if (!reservation.live_at(context.now)) {
        continue;
      }
      line(explanation.rejected, "competing-authority",
           to_string(id) + " held by job " + to_string(reservation.job) + " covers " +
               std::to_string(reservation.targets.size()) + " target(s)");
    }
    for (const auto& [id, other] : context.jobs->jobs()) {
      if (id == job->id) {
        continue;
      }
      if (!other.holds_resources() && other.state != JobState::Proposed) {
        continue;
      }
      line(explanation.rejected, "other-job",
           to_string(id) + " state=" + to_string(other.state) + " priority=" +
               std::to_string(other.priority) + " targets=" + std::to_string(other.targets.size()));
    }
  }

  Digest digest;
  digest.update("mf.explanation.v1");
  digest.update(explanation.subject);
  digest.update(explanation.action);
  digest.update(explanation.outcome);
  digest.update_i64(explanation.at);
  digest.update(to_string(explanation.by));
  digest.update_u64(explanation.policy_revision.value());
  digest.update_u64(explanation.policy_digest);
  digest.update_u64(explanation.topology_revision.value());
  digest.update_u64(explanation.topology_digest);
  digest.update_u64(explanation.readiness_digest);
  digest.update_u64(explanation.impact_digest);
  digest.update_u64(explanation.preconditions_digest);
  const auto fold = [&digest](const std::vector<ExplanationLine>& lines) {
    digest.update_u64(lines.size());
    for (const ExplanationLine& entry : lines) {
      digest.update(entry.code);
      digest.update(entry.text);
    }
  };
  fold(explanation.evidence);
  fold(explanation.policy);
  fold(explanation.lifecycle);
  fold(explanation.rejected);
  explanation.digest = digest.value();
  return explanation;
}

std::string ConflictReport::to_text() const {
  std::string out = "conflict report at ";
  out += format_time(at);
  out += " digest=";
  out += hex_u64(digest);
  out.push_back('\n');
  out += "live reservations:\n";
  if (reservations.empty()) {
    out += "  (none)\n";
  }
  for (const ExplanationLine& entry : reservations) {
    out += "  [";
    out += entry.code;
    out += "] ";
    out += entry.text;
    out.push_back('\n');
  }
  out += "conflicts:\n";
  if (conflicts.empty()) {
    out += "  (none)\n";
  }
  for (const ConflictEntry& entry : conflicts) {
    out += "  ";
    out += to_string(entry.first);
    out += " vs ";
    out += to_string(entry.second);
    out += " on ";
    out += to_string(entry.shared);
    out += " (";
    out += entry.kind;
    out += ") -- ";
    out += entry.detail;
    out.push_back('\n');
  }
  out += "blocked jobs:\n";
  if (blocked.empty()) {
    out += "  (none)\n";
  }
  for (const ExplanationLine& entry : blocked) {
    out += "  [";
    out += entry.code;
    out += "] ";
    out += entry.text;
    out.push_back('\n');
  }
  return out;
}

ConflictReport inspect_conflicts(const ConflictContext& context) {
  ConflictReport report;
  report.at = context.now;
  if (context.jobs == nullptr) {
    return report;
  }

  if (context.ledger != nullptr) {
    for (const auto& [id, reservation] : context.ledger->reservations()) {
      line(report.reservations, to_string(id),
           reservation.to_text() + (reservation.live_at(context.now) ? " live" : " expired"));
    }
  }

  std::vector<const MaintenanceJob*> active;
  for (const auto& [id, job] : context.jobs->jobs()) {
    (void)id;
    if (job.holds_resources() || job.state == JobState::Proposed) {
      active.push_back(&job);
    }
    if (job.state == JobState::Blocked) {
      line(report.blocked, to_string(job.block),
           to_string(job.id) + " -- " + job.block_detail);
    }
  }
  std::sort(active.begin(), active.end(),
            [](const MaintenanceJob* a, const MaintenanceJob* b) { return a->id < b->id; });

  for (std::size_t i = 0; i < active.size(); ++i) {
    for (std::size_t j = i + 1; j < active.size(); ++j) {
      const MaintenanceJob& a = *active[i];
      const MaintenanceJob& b = *active[j];
      for (const TargetId target : a.removal_set) {
        if (std::find(b.removal_set.begin(), b.removal_set.end(), target) == b.removal_set.end()) {
          continue;
        }
        ConflictEntry entry;
        entry.first = a.id;
        entry.second = b.id;
        entry.shared = target;
        entry.kind = "shared-target";
        entry.detail = "both jobs claim " + to_string(target) + " (" + to_string(a.state) +
                       " vs " + to_string(b.state) + ")";
        report.conflicts.push_back(std::move(entry));
        break;
      }
      if (context.topology != nullptr) {
        const std::vector<DomainId> shared_domains = [&]() {
          std::vector<DomainId> out;
          const std::vector<DomainId> da = context.topology->domains_of(a.removal_set);
          const std::vector<DomainId> db = context.topology->domains_of(b.removal_set);
          for (const DomainId domain : da) {
            if (std::find(db.begin(), db.end(), domain) != db.end()) {
              out.push_back(domain);
            }
          }
          return out;
        }();
        for (const DomainId domain : shared_domains) {
          if (context.policy != nullptr && !context.policy->has_domain_limit(domain)) {
            continue;
          }
          ConflictEntry entry;
          entry.first = a.id;
          entry.second = b.id;
          entry.shared = a.removal_set.empty() ? TargetId{} : a.removal_set.front();
          entry.kind = "shared-correlated-domain";
          entry.detail = "both jobs remove capacity from correlated domain " + to_string(domain);
          report.conflicts.push_back(std::move(entry));
        }
      }
    }
    const JobState state = active[i]->state;
    if (state == JobState::Proposed || state == JobState::Validated ||
        state == JobState::Prerequisites || state == JobState::Ready) {
      line(report.queued, to_string(state), active[i]->to_text());
    }
  }

  Digest digest;
  digest.update("mf.conflicts.v1");
  digest.update_i64(report.at);
  digest.update_u64(report.conflicts.size());
  for (const ConflictEntry& entry : report.conflicts) {
    digest.update_u64(entry.first.value());
    digest.update_u64(entry.second.value());
    digest.update_u32(static_cast<std::uint32_t>(entry.shared.kind));
    digest.update_u32(entry.shared.index);
    digest.update(entry.kind);
    digest.update(entry.detail);
  }
  digest.update_u64(report.reservations.size());
  for (const ExplanationLine& entry : report.reservations) {
    digest.update(entry.code);
    digest.update(entry.text);
  }
  digest.update_u64(report.blocked.size());
  for (const ExplanationLine& entry : report.blocked) {
    digest.update(entry.code);
    digest.update(entry.text);
  }
  report.digest = digest.value();
  return report;
}

}  // namespace mf
