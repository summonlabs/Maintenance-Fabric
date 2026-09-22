// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/drain/remote.hpp"

#include <utility>

#include "mf/transport/protocol.hpp"

namespace mf {

RemoteDrainPort::RemoteDrainPort(RemoteDrainOptions options) : options_(std::move(options)) {}

RemoteDrainPort::~RemoteDrainPort() = default;

bool RemoteDrainPort::ensure_connected_locked() {
  if (client_.has_value() && client_->valid()) {
    return true;
  }
  client_.reset();
  for (std::uint32_t attempt = 0; attempt < options_.connect_attempts; ++attempt) {
    Result<TcpClient> connected =
        TcpClient::connect(options_.host, options_.port, options_.deadline);
    if (connected.ok()) {
      client_ = std::move(connected.value());
      return true;
    }
  }
  return false;
}

Result<std::vector<std::byte>> RemoteDrainPort::call(OpCode op,
                                                     const std::vector<std::byte>& body) {
  std::lock_guard<std::mutex> guard(mu_);
  ++calls_;
  if (!ensure_connected_locked()) {
    ++failures_;
    return make_error(ErrorCode::DrainFailed,
                      "drain service at " + options_.host + ":" +
                          std::to_string(options_.port) + " is unreachable");
  }
  RpcEnvelope request_envelope;
  request_envelope.op = static_cast<std::uint16_t>(op);
  request_envelope.body = body;
  Result<Frame> response =
      client_->call_type(FrameType::Request, static_cast<std::uint16_t>(op),
                         encode_envelope(request_envelope), options_.deadline);
  if (!response.ok()) {
    ++failures_;
    client_.reset();
    return response.error();
  }
  if (response.value().type == FrameType::Error) {
    ++failures_;
    const std::string text(reinterpret_cast<const char*>(response.value().payload.data()),
                           response.value().payload.size());
    return make_error(ErrorCode::DrainFailed, "drain service error: " + text);
  }
  Result<RpcEnvelope> envelope = decode_envelope(response.value().payload);
  if (!envelope.ok()) {
    ++failures_;
    return envelope.error();
  }
  if (envelope.value().op != kStatusOk) {
    ++failures_;
    return make_error(ErrorCode::DrainFailed, "drain service rejected the request: " +
                                                  envelope.value().message);
  }
  return envelope.value().body;
}

DrainResponse RemoteDrainPort::request_drain(const DrainRequest& request) {
  DrainResponse response;
  const Status valid = validate_drain_request(request);
  if (!valid.ok()) {
    response.status = valid;
    return response;
  }
  Result<std::vector<std::byte>> body = call(OpCode::DrainRequest, encode_drain_request_body(request));
  if (!body.ok()) {
    response.status = body.error();
    response.lease.state = DrainState::Failed;
    response.lease.targets = request.targets;
    response.lease.attempt = request.attempt;
    response.lease.owner = request.requester;
    return response;
  }
  Result<DrainLease> lease = decode_drain_lease_body(body.value());
  if (!lease.ok()) {
    response.status = lease.error();
    return response;
  }
  response.lease = lease.value();
  response.status = ok_status();
  return response;
}

DrainResponse RemoteDrainPort::refresh_drain(DrainLeaseId lease, Nanos extend,
                                             const ControllerIncarnation& requester) {
  DrainResponse response;
  Result<std::vector<std::byte>> body = call(
      OpCode::DrainRefresh, encode_drain_lease_ref(lease, requester, extend));
  if (!body.ok()) {
    response.status = body.error();
    response.lease.id = lease;
    return response;
  }
  Result<DrainLease> decoded = decode_drain_lease_body(body.value());
  if (!decoded.ok()) {
    response.status = decoded.error();
    return response;
  }
  response.lease = decoded.value();
  response.status = ok_status();
  return response;
}

DrainResponse RemoteDrainPort::query_drain(DrainLeaseId lease,
                                           const ControllerIncarnation& requester) {
  DrainResponse response;
  Result<std::vector<std::byte>> body =
      call(OpCode::DrainQuery, encode_drain_lease_ref(lease, requester, 0));
  if (!body.ok()) {
    response.status = body.error();
    response.lease.id = lease;
    response.lease.state = DrainState::NotFound;
    return response;
  }
  Result<DrainLease> decoded = decode_drain_lease_body(body.value());
  if (!decoded.ok()) {
    response.status = decoded.error();
    return response;
  }
  response.lease = decoded.value();
  response.status = ok_status();
  return response;
}

Status RemoteDrainPort::release_drain(DrainLeaseId lease, const ControllerIncarnation& requester) {
  Result<std::vector<std::byte>> body =
      call(OpCode::DrainRelease, encode_drain_lease_ref(lease, requester, 0));
  if (!body.ok()) {
    return body.error();
  }
  return ok_status();
}

bool RemoteDrainPort::connected() const {
  std::lock_guard<std::mutex> guard(mu_);
  return client_.has_value() && client_->valid();
}

void RemoteDrainPort::disconnect() {
  std::lock_guard<std::mutex> guard(mu_);
  if (client_.has_value()) {
    (void)client_->close();
  }
  client_.reset();
}

std::uint64_t RemoteDrainPort::calls() const {
  std::lock_guard<std::mutex> guard(mu_);
  return calls_;
}

std::uint64_t RemoteDrainPort::failures() const {
  std::lock_guard<std::mutex> guard(mu_);
  return failures_;
}

}  // namespace mf
