// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// mfctl -- operator interface to a Maintenance Fabric controller, either
// in-process against a local journal or remotely against mfd.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "apps/common/api.hpp"
#include "mf/config/text.hpp"
#include "mf/core/log.hpp"
#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"

namespace {

using namespace mf;

struct Args {
  std::vector<std::pair<std::string, std::string>> values;

  [[nodiscard]] bool has(std::string_view name) const {
    for (const auto& [key, value] : values) {
      (void)value;
      if (key == name) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] const std::string* find(std::string_view name) const {
    for (const auto& [key, value] : values) {
      if (key == name) {
        return &value;
      }
    }
    return nullptr;
  }
  [[nodiscard]] std::string get(std::string_view name, std::string fallback = {}) const {
    const std::string* value = find(name);
    return value == nullptr ? std::move(fallback) : *value;
  }
  [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [key, value] : values) {
      if (key == name) {
        out.push_back(value);
      }
    }
    return out;
  }
};

void usage() {
  std::fputs(
      "mfctl -- Maintenance Fabric control interface\n"
      "\n"
      "usage: mfctl [global options] <command> [arguments]\n"
      "\n"
      "global options\n"
      "  --store PATH        local journal path (default mf-state/journal.mfj)\n"
      "  --addr HOST:PORT    talk to a running mfd instead of a local journal\n"
      "  --topology FILE     load a topology before running the command (local mode)\n"
      "  --policy FILE       load a policy before running the command (local mode)\n"
      "  --actor NAME        operator name recorded in the audit trail (default operator)\n"
      "  --now TIME          local mode clock seed (RFC3339 or epoch:seconds)\n"
      "  --advance DURATION  local mode clock step per tick (default 1s)\n"
      "  --log-level LEVEL   error|warn|info|debug|trace (default warn)\n"
      "\n"
      "commands\n"
      "  propose --title T --target ID [--target ID ...] [--priority N] [--reason R]\n"
      "          [--duration D] [--depends JOB] [--no-drain]\n"
      "  approve JOB                     proposed -> validated (arm the request)\n"
      "  arm JOB                         re-arm a blocked job under a new generation\n"
      "  cancel JOB [--reason R]         withdraw a job that has not removed service\n"
      "  start JOB [--max-ticks N]       drive the controller until JOB starts or blocks\n"
      "  tick [--count N]                run scheduler passes\n"
      "  status [JOB]                    show one job or every job\n"
      "  explain JOB [--action A]        deterministic decision explanation\n"
      "  conflicts                       overlapping jobs and live reservations\n"
      "  arbitration                     the most recent arbitration pass\n"
      "  quarantines                     targets withheld from normal service\n"
      "  clear-quarantine ID [--why R]   release a quarantine after inspection\n"
      "  complete JOB --attempt ID [--fail] [--detail D]\n"
      "  restore JOB --attempt ID --target ID [--fail] [--detail D]\n"
      "  observe --target ID --health H --ttl D [--source N]\n"
      "  topology                        print the loaded topology\n"
      "  policy                          print the loaded policy\n"
      "  version                         print the runtime version\n",
      stdout);
}

bool parse_endpoint(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  const std::optional<std::uint32_t> value = parse_u32(text.substr(colon + 1));
  if (!value.has_value() || *value > 65535u) {
    return false;
  }
  host = text.substr(0, colon);
  port = static_cast<std::uint16_t>(*value);
  return true;
}

int fail(const Status& status) {
  std::fprintf(stderr, "mfctl: %s\n", format_error(status.error()).c_str());
  return 1;
}

int fail(const Error& error) {
  std::fprintf(stderr, "mfctl: %s\n", format_error(error).c_str());
  return 1;
}

std::string state_text(const JobSnapshot& snapshot) {
  std::string out = to_string(snapshot.state);
  if (snapshot.block != BlockReason::None) {
    out += "(";
    out += to_string(snapshot.block);
    out.push_back(')');
  }
  return out;
}

void print_table(const std::vector<JobSnapshot>& jobs) {
  std::printf("%-8s %-6s %-6s %-24s %-6s %-10s %s\n", "job", "gen", "rev", "state", "prio",
              "removed", "targets");
  for (const JobSnapshot& snapshot : jobs) {
    std::string targets;
    for (std::size_t i = 0; i < snapshot.targets.size(); ++i) {
      if (i != 0) {
        targets.push_back(',');
      }
      targets += to_string(snapshot.targets[i]);
    }
    const std::string state = state_text(snapshot);
    std::printf("%-8s %-6s %-6s %-24s %-6u %-10s %s\n", to_string(snapshot.id).c_str(),
                to_string(snapshot.generation).c_str(), to_string(snapshot.revision).c_str(),
                state.c_str(), static_cast<unsigned>(snapshot.priority),
                snapshot.service_removed ? "yes" : "no", targets.c_str());
  }
}

int run_command(app::FabricApi& api, const std::string& command, const Args& args,
                const std::string& actor, Nanos advance) {
  if (command == "version") {
    std::printf("%s %s (protocol %u, state format %u)\n", std::string(kProductName).c_str(),
                std::string(kProductVersion).c_str(), static_cast<unsigned>(kProtocolVersion),
                static_cast<unsigned>(kStateFormatVersion));
    return 0;
  }
  if (command == "propose") {
    MaintenanceRequest request;
    request.title = args.get("--title");
    request.reason = args.get("--reason", "operator request");
    if (request.title.empty()) {
      std::fprintf(stderr, "mfctl: --title is required\n");
      return 2;
    }
    for (const std::string& text : args.all("--target")) {
      const std::optional<TargetId> target = parse_target_id(text);
      if (!target.has_value()) {
        std::fprintf(stderr, "mfctl: '%s' is not a target id\n", text.c_str());
        return 2;
      }
      request.targets.push_back(*target);
    }
    if (request.targets.empty()) {
      std::fprintf(stderr, "mfctl: at least one --target is required\n");
      return 2;
    }
    for (const std::string& text : args.all("--depends")) {
      const std::optional<JobId> dependency = parse_id<JobTag>(text);
      if (!dependency.has_value()) {
        std::fprintf(stderr, "mfctl: '%s' is not a job id\n", text.c_str());
        return 2;
      }
      request.depends_on.push_back(*dependency);
    }
    if (const std::string* text = args.find("--priority"); text != nullptr) {
      const std::optional<std::uint32_t> value = parse_u32(*text);
      if (!value.has_value()) {
        std::fprintf(stderr, "mfctl: --priority must be a number\n");
        return 2;
      }
      request.priority = *value;
    }
    if (const std::string* text = args.find("--duration"); text != nullptr) {
      const std::optional<Nanos> value = parse_duration_text(*text);
      if (!value.has_value()) {
        std::fprintf(stderr, "mfctl: --duration must be a duration\n");
        return 2;
      }
      request.estimated_duration = *value;
    }
    request.requires_drain = !args.has("--no-drain");
    request.requestor = requestor_from_name(actor);
    Result<JobId> created = api.propose(request, actor);
    if (!created.ok()) {
      return fail(created.error());
    }
    std::printf("%s\n", to_string(created.value()).c_str());
    return 0;
  }

  if (command == "approve" || command == "arm" || command == "cancel" || command == "start" ||
      command == "status" || command == "explain" || command == "complete" ||
      command == "restore") {
    JobId job;
    bool have_job = false;
    if (const std::string* text = args.find("--job"); text != nullptr) {
      const std::optional<JobId> parsed = parse_id<JobTag>(*text);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "mfctl: --job is not a job id\n");
        return 2;
      }
      job = *parsed;
      have_job = true;
    }
    if (!have_job) {
      std::fprintf(stderr, "mfctl: %s requires a job id\n", command.c_str());
      return 2;
    }
    if (command == "approve") {
      const Status status = api.approve(job, actor);
      if (!status.ok()) {
        return fail(status);
      }
      std::printf("approved %s\n", to_string(job).c_str());
      return 0;
    }
    if (command == "arm") {
      const Status status = api.rearm(job, actor);
      if (!status.ok()) {
        return fail(status);
      }
      std::printf("re-armed %s under a new generation\n", to_string(job).c_str());
      return 0;
    }
    if (command == "cancel") {
      const Status status = api.cancel(job, actor, args.get("--reason", "operator request"));
      if (!status.ok()) {
        return fail(status);
      }
      std::printf("cancelled %s\n", to_string(job).c_str());
      return 0;
    }
    if (command == "status") {
      Result<JobSnapshot> snapshot = api.status(job);
      if (!snapshot.ok()) {
        return fail(snapshot.error());
      }
      print_table({snapshot.value()});
      Result<std::string> explanation = api.explain(job, "status");
      if (explanation.ok()) {
        std::printf("\n%s", explanation.value().c_str());
      }
      return 0;
    }
    if (command == "explain") {
      Result<std::string> explanation = api.explain(job, args.get("--action", "status"));
      if (!explanation.ok()) {
        return fail(explanation.error());
      }
      std::printf("%s", explanation.value().c_str());
      return 0;
    }
    if (command == "start") {
      std::uint32_t max_ticks = 32;
      if (const std::string* text = args.find("--max-ticks"); text != nullptr) {
        const std::optional<std::uint32_t> value = parse_u32(*text);
        if (!value.has_value()) {
          std::fprintf(stderr, "mfctl: --max-ticks must be a number\n");
          return 2;
        }
        max_ticks = *value;
      }
      for (std::uint32_t i = 0; i < max_ticks; ++i) {
        const Status ticked = api.tick(0);
        if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
          return fail(ticked);
        }
        Result<JobSnapshot> snapshot = api.status(job);
        if (!snapshot.ok()) {
          return fail(snapshot.error());
        }
        const JobState state = snapshot.value().state;
        if (state == JobState::InMaintenance || state == JobState::Verifying ||
            state == JobState::Restoring || state == JobState::Complete || state == JobState::Blocked ||
            state == JobState::Failed || state == JobState::Cancelled) {
          break;
        }
      }
      (void)advance;
      Result<JobSnapshot> snapshot = api.status(job);
      if (!snapshot.ok()) {
        return fail(snapshot.error());
      }
      print_table({snapshot.value()});
      Result<std::string> explanation = api.explain(job, "start");
      if (explanation.ok()) {
        std::printf("\n%s", explanation.value().c_str());
      }
      return 0;
    }
    if (command == "complete") {
      const std::string* text = args.find("--attempt");
      if (text == nullptr) {
        std::fprintf(stderr, "mfctl: --attempt is required\n");
        return 2;
      }
      const std::optional<AttemptId> attempt = parse_attempt_id(*text);
      if (!attempt.has_value()) {
        std::fprintf(stderr, "mfctl: --attempt must be a job/generation/attempt id\n");
        return 2;
      }
      const Status status = api.report_completion(job, *attempt, !args.has("--fail"),
                                                  args.get("--detail", "reported by mfctl"));
      if (!status.ok()) {
        return fail(status);
      }
      std::printf("completion recorded for %s\n", to_string(*attempt).c_str());
      return 0;
    }
    if (command == "restore") {
      const std::string* attempt_text = args.find("--attempt");
      const std::string* target_text = args.find("--target");
      if (attempt_text == nullptr || target_text == nullptr) {
        std::fprintf(stderr, "mfctl: --attempt and --target are required\n");
        return 2;
      }
      const std::optional<AttemptId> attempt = parse_attempt_id(*attempt_text);
      const std::optional<TargetId> target = parse_target_id(*target_text);
      if (!attempt.has_value() || !target.has_value()) {
        std::fprintf(stderr, "mfctl: --attempt or --target is malformed\n");
        return 2;
      }
      const Status status = api.report_restoration(job, *attempt, *target, !args.has("--fail"),
                                                   args.get("--detail", "reported by mfctl"));
      if (!status.ok()) {
        return fail(status);
      }
      std::printf("restoration check recorded for %s\n", to_string(*target).c_str());
      return 0;
    }
  }

  if (command == "tick") {
    std::uint32_t count = 1;
    if (const std::string* text = args.find("--count"); text != nullptr) {
      const std::optional<std::uint32_t> value = parse_u32(*text);
      if (!value.has_value()) {
        std::fprintf(stderr, "mfctl: --count must be a number\n");
        return 2;
      }
      count = *value;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const Status ticked = api.tick(0);
      if (!ticked.ok() && ticked.error().code != ErrorCode::ShuttingDown) {
        return fail(ticked);
      }
    }
    std::printf("ran %u scheduler pass(es)\n", static_cast<unsigned>(count));
    return 0;
  }
  if (command == "status" || command == "list") {
    Result<std::vector<JobSnapshot>> jobs = api.list();
    if (!jobs.ok()) {
      return fail(jobs.error());
    }
    print_table(jobs.value());
    return 0;
  }
  if (command == "conflicts") {
    Result<std::string> report = api.conflicts();
    if (!report.ok()) {
      return fail(report.error());
    }
    std::printf("%s", report.value().c_str());
    return 0;
  }
  if (command == "arbitration") {
    Result<std::string> report = api.arbitration();
    if (!report.ok()) {
      return fail(report.error());
    }
    std::printf("%s", report.value().c_str());
    return 0;
  }
  if (command == "quarantines") {
    Result<std::string> report = api.quarantines();
    if (!report.ok()) {
      return fail(report.error());
    }
    std::printf("%s", report.value().c_str());
    return 0;
  }
  if (command == "clear-quarantine") {
    const std::string* text = args.find("--id");
    if (text == nullptr) {
      std::fprintf(stderr, "mfctl: --id is required\n");
      return 2;
    }
    const std::optional<QuarantineId> id = parse_id<QuarantineTag>(*text);
    if (!id.has_value()) {
      std::fprintf(stderr, "mfctl: --id is not a quarantine id\n");
      return 2;
    }
    const Status status = api.clear_quarantine(*id, actor, args.get("--why", "inspected by hand"));
    if (!status.ok()) {
      return fail(status);
    }
    std::printf("quarantine %s cleared\n", to_string(*id).c_str());
    return 0;
  }
  if (command == "observe") {
    const std::string* target_text = args.find("--target");
    if (target_text == nullptr) {
      std::fprintf(stderr, "mfctl: --target is required\n");
      return 2;
    }
    const std::optional<TargetId> target = parse_target_id(*target_text);
    if (!target.has_value()) {
      std::fprintf(stderr, "mfctl: --target is not a target id\n");
      return 2;
    }
    const std::optional<Health> health = parse_health(args.get("--health", "healthy"));
    if (!health.has_value()) {
      std::fprintf(stderr, "mfctl: --health is not a health value\n");
      return 2;
    }
    const std::optional<Nanos> ttl = parse_duration_text(args.get("--ttl", "30s"));
    if (!ttl.has_value()) {
      std::fprintf(stderr, "mfctl: --ttl is not a duration\n");
      return 2;
    }
    EvidenceRecord evidence;
    evidence.key.kind = EvidenceKind::TargetHealth;
    evidence.key.subject = EvidenceSubject::of(*target);
    const std::string* source_text = args.find("--source");
    std::uint32_t source = 1;
    if (source_text != nullptr) {
      const std::optional<std::uint32_t> parsed = parse_u32(*source_text);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "mfctl: --source must be a number\n");
        return 2;
      }
      source = *parsed;
    }
    evidence.key.source = SourceId::from_u64(source);
    evidence.observed_at = SystemClock{}.now();
    if (const std::string* at = args.find("--at"); at != nullptr) {
      const std::optional<Nanos> parsed = parse_time(*at, evidence.observed_at);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "mfctl: --at is not a time\n");
        return 2;
      }
      evidence.observed_at = *parsed;
    }
    evidence.revision = Revision::from_u64(
        static_cast<std::uint64_t>(evidence.observed_at / kNanosPerSecond) + 1);
    evidence.ttl = *ttl;
    evidence.payload.health = *health;
    evidence.payload.result = true;
    const Status status = api.observe(evidence);
    if (!status.ok()) {
      return fail(status);
    }
    std::printf("recorded %s for %s\n", to_string(*health), to_string(*target).c_str());
    return 0;
  }
  if (command == "topology") {
    Result<std::string> text = api.topology_text();
    if (!text.ok()) {
      return fail(text.error());
    }
    std::printf("%s", text.value().c_str());
    return 0;
  }
  if (command == "policy") {
    Result<std::string> text = api.policy_text();
    if (!text.ok()) {
      return fail(text.error());
    }
    std::printf("%s", text.value().c_str());
    return 0;
  }

  std::fprintf(stderr, "mfctl: unknown command '%s'\n", command.c_str());
  usage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string store_path = "mf-state/journal.mfj";
  std::string address;
  std::string topology_path;
  std::string policy_path;
  std::string actor_name = "operator";
  std::string command;
  Nanos clock_seed = 0;
  Nanos advance = kNanosPerSecond;
  LogLevel log_level = LogLevel::Warn;

  Args args;
  bool saw_command = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (!saw_command) {
      if (arg == "--help" || arg == "-h") {
        usage();
        return 0;
      }
      if (arg == "--store") { if (!value(store_path)) return 2; continue; }
      if (arg == "--addr") { if (!value(address)) return 2; continue; }
      if (arg == "--topology") { if (!value(topology_path)) return 2; continue; }
      if (arg == "--policy") { if (!value(policy_path)) return 2; continue; }
      if (arg == "--actor") { if (!value(actor_name)) return 2; continue; }
      if (arg == "--log-level") {
        std::string text;
        if (!value(text) || !parse_log_level(text, log_level)) return 2;
        continue;
      }
      if (arg == "--now") {
        std::string text;
        if (!value(text)) return 2;
        const std::optional<Nanos> parsed = parse_time(text, SystemClock{}.now());
        if (!parsed.has_value()) {
          std::fprintf(stderr, "mfctl: --now is not a time\n");
          return 2;
        }
        clock_seed = *parsed;
        continue;
      }
      if (arg == "--advance") {
        std::string text;
        if (!value(text)) return 2;
        const std::optional<Nanos> parsed = parse_duration_text(text);
        if (!parsed.has_value()) {
          std::fprintf(stderr, "mfctl: --advance is not a duration\n");
          return 2;
        }
        advance = *parsed;
        continue;
      }
      command = arg;
      saw_command = true;
      continue;
    }
    if (arg.rfind("--", 0) == 0) {
      std::string text;
      const bool takes_value = arg != "--no-drain" && arg != "--fail";
      if (takes_value && !value(text)) {
        std::fprintf(stderr, "mfctl: %s requires a value\n", arg.c_str());
        return 2;
      }
      args.values.emplace_back(arg, text);
      continue;
    }
    args.values.emplace_back("--job", arg);
  }

  if (command.empty()) {
    usage();
    return 2;
  }
  Logger::instance().set_level(log_level);

  if (!address.empty()) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(address, host, port)) {
      std::fprintf(stderr, "mfctl: --addr must be HOST:PORT\n");
      return 2;
    }
    Result<std::unique_ptr<app::RemoteApi>> api =
        app::RemoteApi::connect(host, port, limits::kDefaultRpcDeadlineNanos);
    if (!api.ok()) {
      return fail(api.error());
    }
    const std::string target_command = command == "status" && !args.has("--job") ? "list" : command;
    return run_command(*api.value(), target_command, args, actor_name, advance);
  }

  ManualClock clock(clock_seed != 0 ? clock_seed : SystemClock{}.now());
  DrainPortPtr drain = std::make_shared<LocalDrainPort>();
  ControllerOptions options;
  options.store.journal_path = store_path;
  options.name = "mfctl";
  RecoveryReport recovery;
  Result<std::unique_ptr<app::LocalApi>> api =
      app::LocalApi::open(options, drain, clock, recovery);
  if (!api.ok()) {
    return fail(api.error());
  }
  if (log_level >= LogLevel::Info) {
    std::fprintf(stderr, "%s\n", recovery.to_text().c_str());
  }
  if (!topology_path.empty()) {
    const Status loaded = api.value()->load_topology(topology_path);
    if (!loaded.ok()) {
      return fail(loaded);
    }
  }
  if (!policy_path.empty()) {
    const Status loaded = api.value()->load_policy(policy_path, clock.now());
    if (!loaded.ok()) {
      return fail(loaded);
    }
  }
  clock.advance(advance);
  const std::string target_command = command == "status" && !args.has("--job") ? "list" : command;
  return run_command(*api.value(), target_command, args, actor_name, advance);
}
