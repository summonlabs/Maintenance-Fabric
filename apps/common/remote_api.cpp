// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "apps/common/api.hpp"

#include <tuple>
#include <utility>

#include "mf/transport/protocol.hpp"

namespace mf::app {
namespace {

/// The daemon reports failures as the numeric ErrorCode in the envelope opcode,
/// so a remote caller sees exactly the same error code a local caller would.
Result<std::vector<std::byte>> unwrap(const RpcEnvelope& envelope, std::string_view what) {
  if (envelope.op != kStatusOk) {
    const auto code = static_cast<ErrorCode>(envelope.op);
    return make_error(code, std::string(what) + ": " + envelope.message);
  }
  return envelope.body;
}

}  // namespace

Result<std::unique_ptr<RemoteApi>> RemoteApi::connect(const std::string& host, std::uint16_t port,
                                                      Nanos deadline) {
  Result<TcpClient> client = TcpClient::connect(host, port, deadline);
  if (!client.ok()) {
    return client.error();
  }
  std::unique_ptr<RemoteApi> api(new RemoteApi());
  api->client_ = std::make_unique<TcpClient>(std::move(client.value()));
  api->deadline_ = deadline;
  return api;
}

Result<std::vector<std::byte>> RemoteApi::call(OpCode op, const std::vector<std::byte>& body,
                                               std::string_view what) {
  RpcEnvelope envelope;
  envelope.op = static_cast<std::uint16_t>(op);
  envelope.body = body;
  Result<Frame> response = client_->call_type(FrameType::Request, static_cast<std::uint16_t>(op),
                                              encode_envelope(envelope), deadline_);
  if (!response.ok()) {
    return make_error(response.error().code, std::string(what) + ": " + response.error().detail);
  }
  if (response.value().type == FrameType::Error) {
    const std::string text(reinterpret_cast<const char*>(response.value().payload.data()),
                           response.value().payload.size());
    return make_error(ErrorCode::IoError, std::string(what) + ": " + text);
  }
  Result<RpcEnvelope> decoded = decode_envelope(response.value().payload);
  if (!decoded.ok()) {
    return make_error(decoded.error().code, std::string(what) + ": " + decoded.error().detail);
  }
  return unwrap(decoded.value(), what);
}

Status RemoteApi::call_status(OpCode op, const std::vector<std::byte>& body,
                              std::string_view what) {
  Result<std::vector<std::byte>> result = call(op, body, what);
  if (!result.ok()) {
    return result.error();
  }
  return ok_status();
}

Status RemoteApi::load_topology(const std::string& path) {
  (void)path;
  return make_error(ErrorCode::Unsupported,
                    "topology is loaded from the daemon's own configuration files");
}

Status RemoteApi::load_policy(const std::string& path, Nanos now) {
  (void)path;
  (void)now;
  return make_error(ErrorCode::Unsupported,
                    "policy is loaded from the daemon's own configuration files");
}

Result<JobId> RemoteApi::propose(const MaintenanceRequest& request, std::string actor) {
  (void)actor;
  Result<std::vector<std::byte>> body =
      call(OpCode::Propose, encode_request_body(request), "propose");
  if (!body.ok()) {
    return body.error();
  }
  Result<std::pair<JobId, std::string>> parsed = decode_job_ref(body.value());
  if (!parsed.ok()) {
    return parsed.error();
  }
  return parsed.value().first;
}

Status RemoteApi::approve(JobId job, std::string actor) {
  return call_status(OpCode::Approve, encode_job_ref(job, actor), "approve");
}

Status RemoteApi::rearm(JobId job, std::string actor) {
  return call_status(OpCode::Rearm, encode_job_ref(job, actor), "rearm");
}

Status RemoteApi::cancel(JobId job, std::string actor, std::string reason) {
  return call_status(OpCode::Cancel, encode_job_actor(job, actor, reason), "cancel");
}

Status RemoteApi::tick(Nanos now) {
  return call_status(OpCode::Tick, encode_i64_body(now), "tick");
}

Result<JobSnapshot> RemoteApi::status(JobId job) {
  Result<std::vector<std::byte>> body = call(OpCode::Status, encode_job_ref(job, ""), "status");
  if (!body.ok()) {
    return body.error();
  }
  return decode_snapshot(body.value());
}

Result<std::vector<JobSnapshot>> RemoteApi::list() {
  Result<std::vector<std::byte>> body = call(OpCode::List, {}, "list");
  if (!body.ok()) {
    return body.error();
  }
  return decode_snapshot_list(body.value());
}

Result<std::string> RemoteApi::explain(JobId job, std::string action) {
  Result<std::vector<std::byte>> body =
      call(OpCode::Explain, encode_job_ref(job, action), "explain");
  if (!body.ok()) {
    return body.error();
  }
  return decode_text_body(body.value());
}

Result<std::string> RemoteApi::conflicts() {
  Result<std::vector<std::byte>> body = call(OpCode::Conflicts, {}, "conflicts");
  if (!body.ok()) {
    return body.error();
  }
  return decode_text_body(body.value());
}

Result<std::string> RemoteApi::arbitration() {
  Result<std::vector<std::byte>> body = call(OpCode::Ping, {}, "arbitration");
  if (!body.ok()) {
    return body.error();
  }
  return decode_text_body(body.value());
}

Result<std::string> RemoteApi::quarantines() {
  Result<std::vector<std::byte>> body = call(OpCode::Quarantines, {}, "quarantines");
  if (!body.ok()) {
    return body.error();
  }
  return decode_text_body(body.value());
}

Status RemoteApi::clear_quarantine(QuarantineId id, std::string actor, std::string justification) {
  return call_status(OpCode::ClearQuarantine,
                     encode_quarantine_clear(id, actor, justification), "clear-quarantine");
}

Status RemoteApi::report_completion(JobId job, const AttemptId& attempt, bool ok,
                                    std::string detail) {
  return call_status(OpCode::ReportCompletion,
                     encode_completion_report(job, attempt, ok, detail), "report-completion");
}

Status RemoteApi::report_restoration(JobId job, const AttemptId& attempt, TargetId target, bool ok,
                                     std::string detail) {
  return call_status(OpCode::ReportRestoration,
                     encode_restoration_report(job, attempt, target, ok, detail),
                     "report-restoration");
}

Status RemoteApi::observe(const EvidenceRecord& evidence) {
  return call_status(OpCode::Observe, encode_evidence_body(evidence), "observe");
}

Result<std::string> RemoteApi::topology_text() {
  return make_error(ErrorCode::Unsupported, "the daemon does not export its topology");
}

Result<std::string> RemoteApi::policy_text() {
  return make_error(ErrorCode::Unsupported, "the daemon does not export its policy");
}

}  // namespace mf::app
