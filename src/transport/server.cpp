// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/transport/server.hpp"

#include <cstring>
#include <utility>

namespace mf {

TcpServer::TcpServer(Options options, Handler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {
  if (options_.workers == 0) {
    options_.workers = 1;
  }
  if (options_.workers > limits::kMaxServerWorkers) {
    options_.workers = limits::kMaxServerWorkers;
  }
  if (options_.max_queued == 0) {
    options_.max_queued = 1;
  }
  if (options_.max_payload > limits::kMaxFrameBytes) {
    options_.max_payload = limits::kMaxFrameBytes;
  }
}

TcpServer::~TcpServer() { (void)stop(); }

Status TcpServer::start() {
  if (running_.load()) {
    return make_error(ErrorCode::StateConflict, "server is already running");
  }
  Result<Socket> listener =
      Socket::listen_tcp(options_.bind_address, options_.port, static_cast<int>(options_.workers) * 4);
  if (!listener.ok()) {
    return listener.error();
  }
  listener_ = std::move(listener.value());
  port_.store(listener_.local_port());
  {
    std::lock_guard<std::mutex> guard(mu_);
    stopping_ = false;
  }
  running_.store(true);
  acceptor_ = std::thread([this]() { accept_loop(); });
  for (std::uint32_t i = 0; i < options_.workers; ++i) {
    workers_.emplace_back([this]() { worker_loop(); });
  }
  return ok_status();
}

Status TcpServer::stop() {
  {
    std::lock_guard<std::mutex> guard(mu_);
    if (!running_.load() && workers_.empty() && !acceptor_.joinable()) {
      return ok_status();
    }
    stopping_ = true;
  }
  running_.store(false);
  // Releasing the accept call and every live connection happens before any join
  // so no worker can be parked in a blocking read.
  (void)listener_.shutdown_both();
  listener_.close();
  {
    // Shutting down the live connections while holding the lock is safe: the
    // workers unregister under the same lock, so no socket can be destroyed
    // while this loop still holds a pointer to it.
    std::lock_guard<std::mutex> guard(mu_);
    for (Socket* connection : live_) {
      (void)connection->shutdown_both();
    }
    queue_.clear();
  }
  cv_.notify_all();
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> guard(mu_);
    live_.clear();
    queue_.clear();
  }
  return ok_status();
}

void TcpServer::accept_loop() {
  while (true) {
    {
      std::lock_guard<std::mutex> guard(mu_);
      if (stopping_) {
        return;
      }
    }
    Result<Socket> connection = listener_.accept();
    if (!connection.ok()) {
      std::lock_guard<std::mutex> guard(mu_);
      if (stopping_) {
        return;
      }
      continue;
    }
    Socket socket = std::move(connection.value());
    (void)socket.set_recv_timeout(options_.io_timeout);
    (void)socket.set_send_timeout(options_.io_timeout);
    (void)socket.set_no_delay();
    bool admitted = false;
    {
      std::lock_guard<std::mutex> guard(mu_);
      if (!stopping_ && queue_.size() < options_.max_queued) {
        queue_.push_back(std::move(socket));
        admitted = true;
      }
    }
    if (admitted) {
      accepted_.fetch_add(1);
      cv_.notify_one();
    } else {
      rejected_.fetch_add(1);
      (void)socket.shutdown_both();
      socket.close();
    }
  }
}

void TcpServer::worker_loop() {
  while (true) {
    Socket connection;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      connection = std::move(queue_.front());
      queue_.pop_front();
    }
    serve(std::move(connection));
  }
}

void TcpServer::serve(Socket connection) {
  {
    std::lock_guard<std::mutex> guard(mu_);
    live_.insert(&connection);
  }
  while (true) {
    std::vector<std::byte> header(kFrameHeaderBytes);
    const Status got = connection.recv_exact(header);
    if (!got.ok()) {
      break;
    }
    bytes_in_.fetch_add(header.size());
    Result<FrameHeader> decoded = decode_frame_header(header);
    if (!decoded.ok()) {
      failures_.fetch_add(1);
      Frame error;
      error.type = FrameType::Error;
      error.request_id = 0;
      const std::string text = format_error(decoded.error());
      error.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                           reinterpret_cast<const std::byte*>(text.data()) + text.size());
      const std::vector<std::byte> out = encode_frame(error);
      (void)connection.send_all(out);
      bytes_out_.fetch_add(out.size());
      break;
    }
    const FrameHeader head = decoded.value();
    if (head.payload_length > options_.max_payload) {
      failures_.fetch_add(1);
      break;
    }
    std::vector<std::byte> payload(head.payload_length);
    if (head.payload_length > 0) {
      const Status body = connection.recv_exact(payload);
      if (!body.ok()) {
        break;
      }
      bytes_in_.fetch_add(payload.size());
    }
    std::vector<std::byte> raw(kFrameHeaderBytes + payload.size());
    std::memcpy(raw.data(), header.data(), kFrameHeaderBytes);
    if (!payload.empty()) {
      std::memcpy(raw.data() + kFrameHeaderBytes, payload.data(), payload.size());
    }
    Result<Frame> request = decode_frame(raw);
    if (!request.ok()) {
      failures_.fetch_add(1);
      break;
    }
    frames_in_.fetch_add(1);

    Frame response;
    response.request_id = request.value().request_id;
    response.type = FrameType::Response;
    const Status outcome = handler_(request.value(), response);
    if (!outcome.ok()) {
      response.type = FrameType::Error;
      const std::string text = format_error(outcome.error());
      response.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                              reinterpret_cast<const std::byte*>(text.data()) + text.size());
    }
    const std::vector<std::byte> out = encode_frame(response);
    const Status sent = connection.send_all(out);
    if (!sent.ok()) {
      break;
    }
    bytes_out_.fetch_add(out.size());
    frames_out_.fetch_add(1);
    handled_.fetch_add(1);
    if (request.value().type == FrameType::Goodbye) {
      break;
    }
  }
  {
    std::lock_guard<std::mutex> guard(mu_);
    live_.erase(&connection);
  }
  connection.close();
}

ServerStats TcpServer::stats() const {
  ServerStats out;
  out.accepted = accepted_.load();
  out.rejected = rejected_.load();
  out.handled = handled_.load();
  out.failures = failures_.load();
  out.frames_in = frames_in_.load();
  out.frames_out = frames_out_.load();
  out.bytes_in = bytes_in_.load();
  out.bytes_out = bytes_out_.load();
  return out;
}

}  // namespace mf
