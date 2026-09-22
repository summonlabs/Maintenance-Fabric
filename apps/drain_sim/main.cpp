// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// mf-drain-sim -- reference implementation of the FOREIGN side of the drain
// boundary. Maintenance Fabric consumes this service through DrainPort; the
// simulator exists so the framed transport, lease ownership and failure paths
// can be exercised against a real, independent OS process.
//
// This program is not part of the Maintenance Fabric runtime and performs no
// maintenance decision making.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mf/core/log.hpp"
#include "mf/transport/protocol.hpp"
#include "mf/transport/server.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace mf;

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_stop.store(true);
    return TRUE;
  }
  return FALSE;
}
#else
void console_handler(int) { g_stop.store(true); }
#endif

struct Options {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9800};
  std::uint32_t workers{2};
  DrainState granted_state{DrainState::Drained};
  std::uint32_t fail_next{0};
  bool drop_on_query{false};
  bool fence_refresh{true};
  LogLevel log_level{LogLevel::Info};
};

void usage() {
  std::fputs(
      "mf-drain-sim -- reference external drain service for Maintenance Fabric integration\n"
      "\n"
      "usage: mf-drain-sim [options]\n"
      "  --bind ADDRESS       bind address (default 127.0.0.1)\n"
      "  --port PORT          port (default 9800, 0 picks a free port)\n"
      "  --workers N          worker threads (default 2)\n"
      "  --granted-state S    drained | draining (default drained)\n"
      "  --fail-next N        fail the next N drain requests\n"
      "  --drop-on-query      forget every lease at the next query (failure injection)\n"
      "  --allow-foreign-refresh  do not fence refresh by lease owner\n"
      "  --log-level LEVEL    error|warn|info|debug|trace\n"
      "  --help               show this message\n",
      stdout);
}

bool parse_u16(const std::string& text, std::uint16_t& out) {
  const std::optional<std::uint32_t> value = parse_u32(text);
  if (!value.has_value() || *value > 65535u) {
    return false;
  }
  out = static_cast<std::uint16_t>(*value);
  return true;
}

class DrainService {
 public:
  explicit DrainService(Options options) : options_(std::move(options)) {
    fail_next_ = options_.fail_next;
    drop_on_query_ = options_.drop_on_query;
  }

  Status handle(const RpcEnvelope& request, RpcEnvelope& reply) {
    const auto op = static_cast<OpCode>(request.op);
    const std::span<const std::byte> body(request.body);
    switch (op) {
      case OpCode::Ping:
        reply.body = encode_text_body("mf-drain-sim ready");
        return ok_status();
      case OpCode::DrainRequest:
        return request_drain(body, reply);
      case OpCode::DrainRelease:
        return release(body, reply);
      case OpCode::DrainQuery:
        return query(body, reply);
      case OpCode::DrainRefresh:
        return refresh(body, reply);
      default:
        return make_error(ErrorCode::Unsupported, "mf-drain-sim does not serve this operation");
    }
  }

  std::uint64_t leases() const {
    std::lock_guard<std::mutex> guard(mu_);
    return static_cast<std::uint64_t>(leases_.size());
  }
  std::uint64_t granted() const { return granted_.load(); }
  std::uint64_t released() const { return released_.load(); }

 private:
  Status request_drain(std::span<const std::byte> body, RpcEnvelope& reply) {
    Result<DrainRequest> parsed = decode_drain_request_body(body);
    if (!parsed.ok()) {
      return parsed.error();
    }
    const DrainRequest& request = parsed.value();
    const Status valid = validate_drain_request(request);
    if (!valid.ok()) {
      return valid;
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (fail_next_ > 0) {
      --fail_next_;
      return make_error(ErrorCode::DrainFailed, "drain refused by injected failure");
    }
    for (const auto& [id, lease] : leases_) {
      (void)id;
      if (lease.attempt == request.attempt) {
        reply.body = encode_drain_lease_body(lease);
        return ok_status();
      }
    }
    DrainLease lease;
    lease.id = next_id_;
    next_id_ = next_id_.next();
    lease.targets = request.targets;
    lease.granted_at = 0;
    lease.expires_at = request.lease_duration;
    lease.state = options_.granted_state;
    lease.attempt = request.attempt;
    lease.owner = request.requester;
    lease.epoch = ++epoch_;
    leases_.emplace(lease.id, lease);
    granted_.fetch_add(1);
    reply.body = encode_drain_lease_body(lease);
    return ok_status();
  }

  Status release(std::span<const std::byte> body, RpcEnvelope& reply) {
    Result<std::tuple<DrainLeaseId, ControllerIncarnation, Nanos>> parsed =
        decode_drain_lease_ref(body);
    if (!parsed.ok()) {
      return parsed.error();
    }
    const DrainLeaseId id = std::get<0>(parsed.value());
    std::lock_guard<std::mutex> guard(mu_);
    const auto it = leases_.find(id);
    if (it == leases_.end()) {
      return make_error(ErrorCode::NotFound, "drain lease " + to_string(id) + " is not held");
    }
    leases_.erase(it);
    released_.fetch_add(1);
    reply.body = encode_text_body("released " + to_string(id));
    return ok_status();
  }

  Status query(std::span<const std::byte> body, RpcEnvelope& reply) {
    Result<std::tuple<DrainLeaseId, ControllerIncarnation, Nanos>> parsed =
        decode_drain_lease_ref(body);
    if (!parsed.ok()) {
      return parsed.error();
    }
    const DrainLeaseId id = std::get<0>(parsed.value());
    std::lock_guard<std::mutex> guard(mu_);
    if (drop_on_query_) {
      drop_on_query_ = false;
      leases_.clear();
    }
    const auto it = leases_.find(id);
    if (it == leases_.end()) {
      return make_error(ErrorCode::DrainLeaseLost, "drain lease " + to_string(id) + " is lost");
    }
    reply.body = encode_drain_lease_body(it->second);
    return ok_status();
  }

  Status refresh(std::span<const std::byte> body, RpcEnvelope& reply) {
    Result<std::tuple<DrainLeaseId, ControllerIncarnation, Nanos>> parsed =
        decode_drain_lease_ref(body);
    if (!parsed.ok()) {
      return parsed.error();
    }
    const DrainLeaseId id = std::get<0>(parsed.value());
    const ControllerIncarnation& requester = std::get<1>(parsed.value());
    const Nanos extend = std::get<2>(parsed.value());
    std::lock_guard<std::mutex> guard(mu_);
    const auto it = leases_.find(id);
    if (it == leases_.end()) {
      return make_error(ErrorCode::DrainLeaseLost, "drain lease " + to_string(id) + " is lost");
    }
    if (options_.fence_refresh && it->second.owner != requester) {
      return make_error(ErrorCode::StaleIncarnation,
                        "drain lease " + to_string(id) + " is owned by a previous incarnation");
    }
    if (extend <= 0) {
      return make_error(ErrorCode::InvalidArgument, "refresh requires a positive extension");
    }
    it->second.expires_at += extend;
    it->second.owner = requester;
    it->second.epoch = ++epoch_;
    reply.body = encode_drain_lease_body(it->second);
    return ok_status();
  }

  Options options_;
  mutable std::mutex mu_;
  std::map<DrainLeaseId, DrainLease> leases_;
  DrainLeaseId next_id_{DrainLeaseId::from_u64(1)};
  std::uint64_t epoch_{0};
  std::uint32_t fail_next_{0};
  bool drop_on_query_{false};
  std::atomic<std::uint64_t> granted_{0};
  std::atomic<std::uint64_t> released_{0};
};

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--bind") { if (!value(options.bind_address)) return 2; continue; }
    if (arg == "--port") {
      std::string text;
      if (!value(text) || !parse_u16(text, options.port)) return 2;
      continue;
    }
    if (arg == "--workers") {
      std::string text;
      if (!value(text)) return 2;
      const std::optional<std::uint32_t> parsed = parse_u32(text);
      if (!parsed.has_value()) return 2;
      options.workers = *parsed;
      continue;
    }
    if (arg == "--granted-state") {
      std::string text;
      if (!value(text)) return 2;
      const std::optional<DrainState> parsed = parse_drain_state(text);
      if (!parsed.has_value()) return 2;
      options.granted_state = *parsed;
      continue;
    }
    if (arg == "--fail-next") {
      std::string text;
      if (!value(text)) return 2;
      const std::optional<std::uint32_t> parsed = parse_u32(text);
      if (!parsed.has_value()) return 2;
      options.fail_next = *parsed;
      continue;
    }
    if (arg == "--drop-on-query") { options.drop_on_query = true; continue; }
    if (arg == "--allow-foreign-refresh") { options.fence_refresh = false; continue; }
    if (arg == "--log-level") {
      std::string text;
      if (!value(text) || !parse_log_level(text, options.log_level)) return 2;
      continue;
    }
    std::fprintf(stderr, "mf-drain-sim: unknown option '%s'\n", arg.c_str());
    usage();
    return 2;
  }

  Logger::instance().set_level(options.log_level);
  Socket::ensure_initialized();
#ifdef _WIN32
  (void)SetConsoleCtrlHandler(console_handler, TRUE);
#else
  std::signal(SIGINT, console_handler);
  std::signal(SIGTERM, console_handler);
#endif

  DrainService service(options);

  TcpServer::Options server_options;
  server_options.bind_address = options.bind_address;
  server_options.port = options.port;
  server_options.workers = options.workers;

  TcpServer server(server_options, [&service](const Frame& request, Frame& response) -> Status {
    response.type = FrameType::Response;
    Result<RpcEnvelope> envelope = decode_envelope(request.payload);
    RpcEnvelope reply;
    if (!envelope.ok()) {
      reply.op = static_cast<std::uint16_t>(ErrorCode::CorruptState);
      reply.message = format_error(envelope.error());
      response.payload = encode_envelope(reply);
      return ok_status();
    }
    const Status outcome = service.handle(envelope.value(), reply);
    if (!outcome.ok()) {
      reply.op = static_cast<std::uint16_t>(outcome.error().code);
      reply.message = outcome.error().detail;
    } else {
      reply.op = kStatusOk;
      reply.message.clear();
    }
    response.payload = encode_envelope(reply);
    return ok_status();
  });

  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "mf-drain-sim: %s\n", format_error(started.error()).c_str());
    return 1;
  }
  std::printf("mf-drain-sim listening on %s:%u\n", options.bind_address.c_str(),
              static_cast<unsigned>(server.port()));
  std::fflush(stdout);

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  (void)server.stop();
  std::printf("mf-drain-sim stopped: granted=%llu released=%llu held=%llu\n",
              static_cast<unsigned long long>(service.granted()),
              static_cast<unsigned long long>(service.released()),
              static_cast<unsigned long long>(service.leases()));
  return 0;
}
