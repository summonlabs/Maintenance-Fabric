// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// mfd -- the Maintenance Fabric daemon. Hosts the controller, the durable store
// and the framed RPC surface. It never performs maintenance work itself.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "apps/common/api.hpp"
#include "mf/config/text.hpp"
#include "mf/core/log.hpp"
#include "mf/drain/local.hpp"
#include "mf/drain/remote.hpp"
#include "mf/runtime/controller.hpp"
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
  std::string store_path{"mf-state/journal.mfj"};
  std::string topology_path;
  std::string policy_path;
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9700};
  std::string drain_address;
  std::string drain_bind{"127.0.0.1"};
  std::uint16_t drain_port{0};
  Nanos tick_interval{kNanosPerSecond};
  std::uint32_t workers{4};
  LogLevel log_level{LogLevel::Info};
  bool static_health{false};
  bool once{false};
};

void usage() {
  std::fputs(
      "mfd -- Maintenance Fabric daemon\n"
      "\n"
      "usage: mfd [options]\n"
      "\n"
      "  --store PATH          journal path (default mf-state/journal.mfj)\n"
      "  --topology FILE       load a topology description at startup\n"
      "  --policy FILE         load a policy description at startup\n"
      "  --bind ADDRESS        RPC bind address (default 127.0.0.1)\n"
      "  --port PORT           RPC port (default 9700, 0 picks a free port)\n"
      "  --workers N           RPC worker threads (default 4)\n"
      "  --tick-interval D     scheduler tick interval, e.g. 1s (default 1s)\n"
      "  --drain-address H:P   use a real external drain service (default: in-process reference)\n"
      "  --drain-bind ADDRESS  bind address for the in-process reference drain service\n"
      "  --health-provider K   none | static-healthy (lab convenience, default none)\n"
      "  --log-level LEVEL     error|warn|info|debug|trace (default info)\n"
      "  --once                serve exactly one request then exit (diagnostics)\n"
      "  --help                show this message\n",
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

std::optional<std::pair<std::string, std::uint16_t>> parse_endpoint(const std::string& text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    return std::nullopt;
  }
  std::uint16_t port = 0;
  if (!parse_u16(text.substr(colon + 1), port)) {
    return std::nullopt;
  }
  return std::make_pair(text.substr(0, colon), port);
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto next = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    }
    if (arg == "--store") {
      if (!next(options.store_path)) return false;
    } else if (arg == "--topology") {
      if (!next(options.topology_path)) return false;
    } else if (arg == "--policy") {
      if (!next(options.policy_path)) return false;
    } else if (arg == "--bind") {
      if (!next(options.bind_address)) return false;
    } else if (arg == "--workers") {
      std::string text;
      if (!next(text)) return false;
      const std::optional<std::uint32_t> value = parse_u32(text);
      if (!value.has_value()) return false;
      options.workers = *value;
    } else if (arg == "--port") {
      std::string text;
      if (!next(text) || !parse_u16(text, options.port)) return false;
    } else if (arg == "--tick-interval") {
      std::string text;
      if (!next(text)) return false;
      const std::optional<Nanos> value = parse_duration_text(text);
      if (!value.has_value()) return false;
      options.tick_interval = *value;
    } else if (arg == "--drain-address") {
      if (!next(options.drain_address)) return false;
    } else if (arg == "--drain-bind") {
      if (!next(options.drain_bind)) return false;
    } else if (arg == "--drain-port") {
      std::string text;
      if (!next(text) || !parse_u16(text, options.drain_port)) return false;
    } else if (arg == "--health-provider") {
      std::string text;
      if (!next(text)) return false;
      if (text == "none") {
        options.static_health = false;
      } else if (text == "static-healthy") {
        options.static_health = true;
      } else {
        std::fprintf(stderr, "mfd: unknown health provider '%s'\n", text.c_str());
        return false;
      }
    } else if (arg == "--log-level") {
      std::string text;
      if (!next(text) || !parse_log_level(text, options.log_level)) return false;
    } else if (arg == "--once") {
      options.once = true;
    } else {
      std::fprintf(stderr, "mfd: unknown option '%s'\n", arg.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
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

  DrainPortPtr drain;
  if (options.drain_address.empty()) {
    drain = std::make_shared<LocalDrainPort>();
    MF_LOG(Info, "mfd",
           "using the in-process reference drain service; production deployments point "
           "--drain-address at a real Drain Fabric endpoint");
  } else {
    const std::optional<std::pair<std::string, std::uint16_t>> endpoint =
        parse_endpoint(options.drain_address);
    if (!endpoint.has_value()) {
      std::fprintf(stderr, "mfd: --drain-address must be HOST:PORT\n");
      return 2;
    }
    RemoteDrainOptions remote;
    remote.host = endpoint->first;
    remote.port = endpoint->second;
    drain = std::make_shared<RemoteDrainPort>(remote);
  }

  SystemClock clock;
  ControllerOptions controller_options;
  controller_options.store.journal_path = options.store_path;
  controller_options.name = "mfd";
  RecoveryReport recovery;
  Result<std::unique_ptr<app::LocalApi>> api =
      app::LocalApi::open(controller_options, drain, clock, recovery);
  if (!api.ok()) {
    std::fprintf(stderr, "mfd: cannot open the controller: %s\n",
                 format_error(api.error()).c_str());
    return 1;
  }
  MF_LOG(Info, "mfd", recovery.to_text());
  MF_LOG(Info, "mfd",
         std::string("incarnation ") + to_string(api.value()->controller().incarnation()));

  if (!options.topology_path.empty()) {
    const Status loaded = api.value()->load_topology(options.topology_path);
    if (!loaded.ok()) {
      std::fprintf(stderr, "mfd: %s\n", format_error(loaded.error()).c_str());
      return 1;
    }
  }
  if (!options.policy_path.empty()) {
    const Status loaded = api.value()->load_policy(options.policy_path, clock.now());
    if (!loaded.ok()) {
      std::fprintf(stderr, "mfd: %s\n", format_error(loaded.error()).c_str());
      return 1;
    }
  }

  Controller& controller = api.value()->controller();
  std::atomic<std::uint64_t> requests{0};

  TcpServer::Options server_options;
  server_options.bind_address = options.bind_address;
  server_options.port = options.port;
  server_options.workers = options.workers;

  const auto handler = [&controller, &requests](const Frame& request, Frame& response) -> Status {
    requests.fetch_add(1);
    response.type = FrameType::Response;
    Result<RpcEnvelope> envelope = decode_envelope(request.payload);
    RpcEnvelope reply;
    if (!envelope.ok()) {
      reply.op = static_cast<std::uint16_t>(ErrorCode::CorruptState);
      reply.message = format_error(envelope.error());
      response.payload = encode_envelope(reply);
      return ok_status();
    }
    const auto op = static_cast<OpCode>(envelope.value().op);
    const std::span<const std::byte> body(envelope.value().body);
    Status outcome = ok_status();
    switch (op) {
      case OpCode::Ping:
        reply.body = encode_text_body(controller.last_arbitration().to_text());
        break;
      case OpCode::Status: {
        Result<std::pair<JobId, std::string>> ref = decode_job_ref(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        Result<JobSnapshot> snapshot = controller.status(ref.value().first);
        if (!snapshot.ok()) {
          outcome = snapshot.error();
          break;
        }
        reply.body = encode_snapshot(snapshot.value());
        break;
      }
      case OpCode::List: {
        std::vector<JobSnapshot> snapshots = controller.list();
        reply.body = encode_snapshot_list(snapshots);
        break;
      }
      case OpCode::Propose: {
        Result<MaintenanceRequest> parsed = decode_request_body(body);
        if (!parsed.ok()) {
          outcome = parsed.error();
          break;
        }
        MaintenanceRequest decoded = parsed.value();
        if (decoded.requestor == RequestorId{}) {
          decoded.requestor = requestor_from_name("remote");
        }
        Result<JobId> created = controller.propose(decoded, "remote");
        if (!created.ok()) {
          outcome = created.error();
          break;
        }
        reply.body = encode_job_ref(created.value(), "");
        break;
      }
      case OpCode::Approve: {
        Result<std::pair<JobId, std::string>> ref = decode_job_ref(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        outcome = controller.approve(ref.value().first, ref.value().second);
        break;
      }
      case OpCode::Rearm: {
        Result<std::pair<JobId, std::string>> ref = decode_job_ref(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        outcome = controller.rearm(ref.value().first, ref.value().second);
        break;
      }
      case OpCode::Cancel: {
        Result<std::tuple<JobId, std::string, std::string>> ref = decode_job_actor(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        outcome = controller.cancel(std::get<0>(ref.value()), std::get<1>(ref.value()),
                                    std::get<2>(ref.value()));
        break;
      }
      case OpCode::Tick: {
        Result<std::int64_t> at = decode_i64_body(body);
        if (!at.ok()) {
          outcome = at.error();
          break;
        }
        outcome = controller.tick(static_cast<Nanos>(at.value()));
        break;
      }
      case OpCode::Explain: {
        Result<std::pair<JobId, std::string>> ref = decode_job_ref(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        Result<Explanation> explanation = controller.explain(ref.value().first, ref.value().second);
        if (!explanation.ok()) {
          outcome = explanation.error();
          break;
        }
        reply.body = encode_text_body(explanation.value().to_text());
        break;
      }
      case OpCode::Conflicts:
        reply.body = encode_text_body(controller.conflicts().to_text());
        break;
      case OpCode::Quarantines: {
        std::string text;
        for (const QuarantineRecord& record : controller.quarantines()) {
          text += to_string(record.id);
          text += " target=";
          text += to_string(record.target);
          text += " job=";
          text += to_string(record.job);
          text += " reason=";
          text += to_string(record.reason);
          text += std::string(" cleared=") + (record.cleared ? "yes" : "no");
          text += " detail=";
          text += record.detail;
          text.push_back('\n');
        }
        if (text.empty()) {
          text = "no quarantined targets\n";
        }
        reply.body = encode_text_body(text);
        break;
      }
      case OpCode::ClearQuarantine: {
        Result<std::tuple<QuarantineId, std::string, std::string>> ref =
            decode_quarantine_clear(body);
        if (!ref.ok()) {
          outcome = ref.error();
          break;
        }
        outcome = controller.clear_quarantine(std::get<0>(ref.value()), std::get<1>(ref.value()),
                                              std::get<2>(ref.value()));
        break;
      }
      case OpCode::ReportCompletion: {
        Result<std::tuple<JobId, AttemptId, bool, std::string>> parsed =
            decode_completion_report(body);
        if (!parsed.ok()) {
          outcome = parsed.error();
          break;
        }
        outcome = controller.report_completion(std::get<0>(parsed.value()),
                                               std::get<1>(parsed.value()),
                                               std::get<2>(parsed.value()),
                                               std::get<3>(parsed.value()));
        break;
      }
      case OpCode::ReportRestoration: {
        Result<std::tuple<JobId, AttemptId, TargetId, bool, std::string>> parsed =
            decode_restoration_report(body);
        if (!parsed.ok()) {
          outcome = parsed.error();
          break;
        }
        outcome = controller.report_restoration(
            std::get<0>(parsed.value()), std::get<1>(parsed.value()), std::get<2>(parsed.value()),
            std::get<3>(parsed.value()), std::get<4>(parsed.value()));
        break;
      }
      case OpCode::Observe: {
        Result<EvidenceRecord> parsed = decode_evidence_body(body);
        if (!parsed.ok()) {
          outcome = parsed.error();
          break;
        }
        outcome = controller.observe(parsed.value());
        break;
      }
      case OpCode::DrainRequest:
      case OpCode::DrainRelease:
      case OpCode::DrainQuery:
      case OpCode::DrainRefresh:
        outcome = make_error(ErrorCode::Unsupported,
                             "mfd is not a drain service; use the Drain Fabric endpoint");
        break;
    }
    if (!outcome.ok()) {
      reply.op = static_cast<std::uint16_t>(outcome.error().code);
      reply.message = outcome.error().detail;
    } else {
      reply.op = kStatusOk;
      reply.message.clear();
    }
    response.payload = encode_envelope(reply);
    return ok_status();
  };

  TcpServer server(server_options, handler);
  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "mfd: %s\n", format_error(started.error()).c_str());
    return 1;
  }
  std::printf("mfd listening on %s:%u\n", options.bind_address.c_str(),
              static_cast<unsigned>(server.port()));
  std::fflush(stdout);

  std::thread ticker([&controller, &options]() {
    while (!g_stop.load()) {
      const Status ticked = controller.tick();
      if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
        MF_LOG(Warn, "mfd", std::string("tick failed: ") + format_error(ticked.error()));
      }
      Nanos slept = 0;
      while (slept < options.tick_interval && !g_stop.load()) {
        const Nanos slice = 20000000;  // 20ms
        std::this_thread::sleep_for(std::chrono::nanoseconds(slice));
        slept += slice;
      }
    }
  });

  std::uint64_t health_revision = 0;
  const auto publish_health = [&controller, &health_revision]() {
    const Topology& topology = controller.topology();
    const Nanos now = SystemClock{}.now();
    for (const auto& [id, record] : topology.targets()) {
      (void)record;
      EvidenceRecord evidence;
      evidence.key.kind = EvidenceKind::TargetHealth;
      evidence.key.subject = EvidenceSubject::of(id);
      evidence.key.source = SourceId::from_u64(1);
      evidence.revision = Revision::from_u64(++health_revision);
      evidence.observed_at = now;
      evidence.ttl = controller.policy().ttl_for(EvidenceKind::TargetHealth);
      evidence.klass = EvidenceClass::Volatile;
      evidence.payload.health = Health::Healthy;
      evidence.payload.result = true;
      (void)controller.observe(evidence);
    }
  };

  // Publish the first health snapshot before entering the loop so a job
  // proposed immediately after startup sees fresh evidence.
  if (options.static_health) {
    publish_health();
  }
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (options.static_health) {
      publish_health();
    }
    if (options.once && requests.load() > 0) {
      break;
    }
  }

  g_stop.store(true);
  if (ticker.joinable()) {
    ticker.join();
  }
  (void)server.stop();
  (void)controller.shutdown();
  std::printf("mfd stopped: handled=%llu rejected=%llu\n",
              static_cast<unsigned long long>(server.stats().handled),
              static_cast<unsigned long long>(server.stats().rejected));
  return 0;
}
