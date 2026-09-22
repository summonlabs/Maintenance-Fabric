// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "apps/common/api.hpp"

#include <utility>

#include "mf/config/text.hpp"

namespace mf::app {

FabricApi::~FabricApi() = default;

LocalApi::LocalApi(std::unique_ptr<Controller> controller) : controller_(std::move(controller)) {}

Result<std::unique_ptr<LocalApi>> LocalApi::open(const ControllerOptions& options,
                                                 DrainPortPtr drain, const Clock& clock,
                                                 RecoveryReport& report) {
  Result<std::unique_ptr<Controller>> controller =
      Controller::open(options, std::move(drain), clock, report);
  if (!controller.ok()) {
    return controller.error();
  }
  return std::unique_ptr<LocalApi>(new LocalApi(std::move(controller.value())));
}

Status LocalApi::load_topology(const std::string& path) {
  ParseStats stats;
  Result<Topology> topology = load_topology_file(path, stats);
  if (!topology.ok()) {
    return topology.error();
  }
  return controller_->set_topology(std::move(topology.value()));
}

Status LocalApi::load_policy(const std::string& path, Nanos now) {
  ParseStats stats;
  Result<Policy> policy = load_policy_file(path, now, stats);
  if (!policy.ok()) {
    return policy.error();
  }
  return controller_->set_policy(std::move(policy.value()));
}

Result<JobId> LocalApi::propose(const MaintenanceRequest& request, std::string actor) {
  return controller_->propose(request, std::move(actor));
}

Status LocalApi::approve(JobId job, std::string actor) {
  return controller_->approve(job, std::move(actor));
}

Status LocalApi::rearm(JobId job, std::string actor) {
  return controller_->rearm(job, std::move(actor));
}

Status LocalApi::cancel(JobId job, std::string actor, std::string reason) {
  return controller_->cancel(job, std::move(actor), std::move(reason));
}

Status LocalApi::tick(Nanos now) { return controller_->tick(now); }

Result<JobSnapshot> LocalApi::status(JobId job) { return controller_->status(job); }

Result<std::vector<JobSnapshot>> LocalApi::list() { return controller_->list(); }

Result<std::string> LocalApi::explain(JobId job, std::string action) {
  Result<Explanation> explanation = controller_->explain(job, std::move(action));
  if (!explanation.ok()) {
    return explanation.error();
  }
  return explanation.value().to_text();
}

Result<std::string> LocalApi::conflicts() { return controller_->conflicts().to_text(); }

Result<std::string> LocalApi::arbitration() {
  return controller_->last_arbitration().to_text();
}

Result<std::string> LocalApi::quarantines() {
  std::string out;
  for (const QuarantineRecord& record : controller_->quarantines()) {
    out += to_string(record.id);
    out += " target=";
    out += to_string(record.target);
    out += " job=";
    out += to_string(record.job);
    out += " reason=";
    out += to_string(record.reason);
    out += std::string(" cleared=") + (record.cleared ? "yes" : "no");
    out += " detail=";
    out += record.detail;
    out.push_back('\n');
  }
  if (out.empty()) {
    out = "no quarantined targets\n";
  }
  return out;
}

Status LocalApi::clear_quarantine(QuarantineId id, std::string actor, std::string justification) {
  return controller_->clear_quarantine(id, std::move(actor), std::move(justification));
}

Status LocalApi::report_completion(JobId job, const AttemptId& attempt, bool ok,
                                   std::string detail) {
  return controller_->report_completion(job, attempt, ok, std::move(detail));
}

Status LocalApi::report_restoration(JobId job, const AttemptId& attempt, TargetId target, bool ok,
                                    std::string detail) {
  return controller_->report_restoration(job, attempt, target, ok, std::move(detail));
}

Status LocalApi::observe(const EvidenceRecord& evidence) { return controller_->observe(evidence); }

Result<std::string> LocalApi::topology_text() { return render_topology(controller_->topology()); }

Result<std::string> LocalApi::policy_text() { return render_policy(controller_->policy()); }

}  // namespace mf::app
