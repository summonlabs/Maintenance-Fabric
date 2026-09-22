// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/transport/client.hpp"

#include <chrono>
#include <cstring>
#include <utility>

namespace mf {

Result<TcpClient> TcpClient::connect(std::string_view host, std::uint16_t port, Nanos timeout) {
  Result<Socket> socket = Socket::connect_tcp(host, port, timeout);
  if (!socket.ok()) {
    return socket.error();
  }
  TcpClient client;
  client.socket_ = std::move(socket.value());
  return client;
}

Result<Frame> TcpClient::call(const Frame& request, Nanos deadline) {
  const Status valid = validate_frame(request);
  if (!valid.ok()) {
    return valid.error();
  }
  if (!socket_.valid()) {
    return make_error(ErrorCode::IoError, "client is not connected");
  }
  const Nanos bounded = deadline <= 0 || deadline > limits::kMaxRpcDeadlineNanos
                            ? limits::kDefaultRpcDeadlineNanos
                            : deadline;
  (void)socket_.set_recv_timeout(bounded);
  (void)socket_.set_send_timeout(bounded);
  const std::vector<std::byte> out = encode_frame(request);
  const Status sent = socket_.send_all(out);
  if (!sent.ok()) {
    return sent.error();
  }
  std::vector<std::byte> header(kFrameHeaderBytes);
  const Status got = socket_.recv_exact(header);
  if (!got.ok()) {
    return got.error();
  }
  Result<FrameHeader> decoded = decode_frame_header(header);
  if (!decoded.ok()) {
    return decoded.error();
  }
  if (decoded.value().request_id != request.request_id) {
    return make_error(ErrorCode::CorruptState,
                      "response request id " + std::to_string(decoded.value().request_id) +
                          " does not match request id " + std::to_string(request.request_id));
  }
  std::vector<std::byte> raw;
  raw.reserve(kFrameHeaderBytes + decoded.value().payload_length);
  raw.insert(raw.end(), header.begin(), header.end());
  if (decoded.value().payload_length > 0) {
    std::vector<std::byte> body(decoded.value().payload_length);
    const Status body_status = socket_.recv_exact(body);
    if (!body_status.ok()) {
      return body_status.error();
    }
    raw.insert(raw.end(), body.begin(), body.end());
  }
  return decode_frame(raw);
}

Result<Frame> TcpClient::call_type(FrameType type, std::uint16_t op,
                                   const std::vector<std::byte>& body, Nanos deadline) {
  Frame request;
  request.type = type;
  request.request_id = next_request_id();
  request.payload = body;
  request.flags = op;
  return call(request, deadline);
}

Status TcpClient::close() {
  if (!socket_.valid()) {
    return ok_status();
  }
  Frame goodbye;
  goodbye.type = FrameType::Goodbye;
  goodbye.request_id = next_request_id();
  const std::vector<std::byte> out = encode_frame(goodbye);
  (void)socket_.send_all(out);
  (void)socket_.shutdown_both();
  socket_.close();
  return ok_status();
}

}  // namespace mf
