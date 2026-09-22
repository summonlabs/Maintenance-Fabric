#pragma once

// Shared test support: temporary directories, deterministic fixtures and real
// child-process control for the transport tests. No test uses a timeout to hide
// a hang; the only bounded waits here are process-startup synchronisation.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"
#include "mf/runtime/controller.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mftest {

// --- temporary directories ---------------------------------------------------

class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mf-test-" + tag + "-" + std::to_string(static_cast<long long>(now)));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] std::string file(const std::string& name) const {
    return (path_ / name).string();
  }
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

// --- child processes ---------------------------------------------------------

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { (void)kill_and_wait(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      (void)kill_and_wait();
#ifdef _WIN32
      process_ = other.process_;
      read_pipe_ = other.read_pipe_;
      other.process_ = PROCESS_INFORMATION{};
      other.read_pipe_ = nullptr;
#else
      pid_ = other.pid_;
      read_fd_ = other.read_fd_;
      other.pid_ = -1;
      other.read_fd_ = -1;
#endif
      buffer_ = std::move(other.buffer_);
      exited_ = other.exited_;
      exit_code_ = other.exit_code_;
      other.exited_ = true;
    }
    return *this;
  }

  [[nodiscard]] static std::optional<ChildProcess> spawn(const std::string& executable,
                                                         const std::vector<std::string>& args,
                                                         bool capture_stdout);

  /// Reads one stdout line, waiting up to \p budget for it.
  [[nodiscard]] std::optional<std::string> read_line(mf::Nanos budget);

  void terminate();
  [[nodiscard]] bool running() const;
  [[nodiscard]] int wait();
  [[nodiscard]] int kill_and_wait();

 private:
#ifdef _WIN32
  PROCESS_INFORMATION process_{};
  HANDLE read_pipe_{nullptr};
#else
  int pid_{-1};
  int read_fd_{-1};
#endif
  std::string buffer_;
  bool exited_{false};
  int exit_code_{-1};
};

inline std::optional<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                                       const std::vector<std::string>& args,
                                                       bool capture_stdout) {
  std::string command_line = "\"" + executable + "\"";
  for (const std::string& arg : args) {
    command_line += " \"" + arg + "\"";
  }
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (capture_stdout) {
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
      return std::nullopt;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  if (capture_stdout) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  ChildProcess child;
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');
  const BOOL created =
      CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, capture_stdout ? TRUE : FALSE,
                     0, nullptr, nullptr, &startup, &child.process_);
  if (capture_stdout) {
    CloseHandle(write_end);
  }
  if (created == 0) {
    if (capture_stdout) {
      CloseHandle(read_end);
    }
    return std::nullopt;
  }
  child.read_pipe_ = capture_stdout ? read_end : nullptr;
  return child;
#else
  int fds[2] = {-1, -1};
  if (capture_stdout && ::pipe(fds) != 0) {
    return std::nullopt;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::nullopt;
  }
  if (pid == 0) {
    if (capture_stdout) {
      ::dup2(fds[1], STDOUT_FILENO);
      ::dup2(fds[1], STDERR_FILENO);
      ::close(fds[0]);
      ::close(fds[1]);
    }
    std::vector<char*> argv;
    std::string mutable_exe = executable;
    argv.push_back(mutable_exe.data());
    std::vector<std::string> mutable_args = args;
    for (std::string& arg : mutable_args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ChildProcess child;
  child.pid_ = static_cast<int>(pid);
  if (capture_stdout) {
    ::close(fds[1]);
    child.read_fd_ = fds[0];
  }
  return child;
#endif
}

inline std::optional<std::string> ChildProcess::read_line(mf::Nanos budget) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(static_cast<long long>(budget));
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
      }
      return line;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return std::nullopt;
    }
    char chunk[512];
#ifdef _WIN32
    if (read_pipe_ == nullptr) {
      return std::nullopt;
    }
    DWORD available = 0;
    if (PeekNamedPipe(read_pipe_, nullptr, 0, nullptr, &available, nullptr) == 0) {
      return std::nullopt;
    }
    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    DWORD got = 0;
    if (ReadFile(read_pipe_, chunk, sizeof(chunk), &got, nullptr) == 0 || got == 0) {
      return std::nullopt;
    }
    buffer_.append(chunk, got);
#else
    if (read_fd_ < 0) {
      return std::nullopt;
    }
    const ssize_t got = ::read(read_fd_, chunk, sizeof(chunk));
    if (got <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    buffer_.append(chunk, static_cast<std::size_t>(got));
#endif
  }
}

inline void ChildProcess::terminate() {
#ifdef _WIN32
  if (process_.hProcess != nullptr && !exited_) {
    (void)TerminateProcess(process_.hProcess, 0xDEADu);
  }
#else
  if (pid_ > 0 && !exited_) {
    (void)::kill(pid_, SIGKILL);
  }
#endif
}

inline bool ChildProcess::running() const {
  if (exited_) {
    return false;
  }
#ifdef _WIN32
  if (process_.hProcess == nullptr) {
    return false;
  }
  return WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT;
#else
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  return ::waitpid(pid_, &status, WNOHANG) == 0;
#endif
}

inline int ChildProcess::wait() {
  if (exited_) {
    return exit_code_;
  }
#ifdef _WIN32
  if (process_.hProcess == nullptr) {
    return -1;
  }
  (void)WaitForSingleObject(process_.hProcess, INFINITE);
  DWORD code = 0;
  (void)GetExitCodeProcess(process_.hProcess, &code);
  if (read_pipe_ != nullptr) {
    CloseHandle(read_pipe_);
    read_pipe_ = nullptr;
  }
  CloseHandle(process_.hThread);
  CloseHandle(process_.hProcess);
  process_.hProcess = nullptr;
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  (void)::waitpid(pid_, &status, 0);
  if (read_fd_ >= 0) {
    ::close(read_fd_);
    read_fd_ = -1;
  }
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  exited_ = true;
  return exit_code_;
}

inline int ChildProcess::kill_and_wait() {
  terminate();
  return wait();
}

// --- fixtures ----------------------------------------------------------------

/// Four switches, two links each, in separate rack and power domains. Link j of
/// switch i has index i*2+j; pool k holds the k-th link of every switch.
inline mf::Topology make_topology(std::size_t switches = 4, std::size_t links_per_switch = 2) {
  using namespace mf;
  Topology topology;
  DomainRecord site;
  site.id = DomainId{DomainKind::Site, 1};
  site.name = "site-1";
  (void)topology.add_domain(site);
  DomainRecord pod;
  pod.id = DomainId{DomainKind::Pod, 1};
  pod.name = "pod-1";
  (void)topology.add_domain(pod);

  for (std::size_t i = 0; i < switches; ++i) {
    DomainRecord rack;
    rack.id = DomainId{DomainKind::Rack, static_cast<std::uint32_t>(i + 1)};
    rack.name = "rack-" + std::to_string(i + 1);
    (void)topology.add_domain(rack);
    DomainRecord power;
    power.id = DomainId{DomainKind::Power, static_cast<std::uint32_t>(i + 1)};
    power.name = "feed-" + std::to_string(i + 1);
    (void)topology.add_domain(power);

    TargetRecord sw;
    sw.id = TargetId{TargetKind::Switch, static_cast<std::uint32_t>(i + 1)};
    sw.name = "sw-" + std::to_string(i + 1);
    sw.domains = {site.id, pod.id, rack.id, power.id};
    (void)topology.add_target(sw);

    for (std::size_t j = 0; j < links_per_switch; ++j) {
      TargetRecord link;
      link.id = TargetId{TargetKind::Link, static_cast<std::uint32_t>(i * links_per_switch + j + 1)};
      link.name = "lag-" + std::to_string(i + 1) + "-" + std::to_string(j + 1);
      link.parent = sw.id;
      link.domains = {site.id, pod.id, rack.id, power.id};
      (void)topology.add_target(link);
    }
  }
  for (std::size_t j = 0; j < links_per_switch; ++j) {
    PoolRecord pool;
    pool.id = PoolId::from_u64(j + 1);
    pool.name = "uplink-" + std::to_string(j + 1);
    for (std::size_t i = 0; i < switches; ++i) {
      pool.members.push_back(
          TargetId{TargetKind::Link, static_cast<std::uint32_t>(i * links_per_switch + j + 1)});
    }
    (void)topology.add_pool(pool);
  }
  TargetRecord control;
  control.id = TargetId{TargetKind::ControlPlane, 1};
  control.name = "cp-1";
  control.domains = {site.id, pod.id};
  (void)topology.add_target(control);
  TargetRecord control2;
  control2.id = TargetId{TargetKind::ControlPlane, 2};
  control2.name = "cp-2";
  control2.domains = {site.id, pod.id, DomainId{DomainKind::Zone, 1}};
  (void)topology.add_domain(DomainRecord{DomainId{DomainKind::Zone, 1}, "zone-1", true});
  (void)topology.add_target(control2);
  return topology;
}

/// N+K redundancy per pool plus one-out-per-rack correlated domain limits.
inline mf::Policy make_policy(std::uint32_t n_plus = 1, std::size_t pools = 2) {
  using namespace mf;
  Policy policy;
  for (std::size_t j = 0; j < pools; ++j) {
    RedundancyRule rule;
    rule.pool = PoolId::from_u64(j + 1);
    rule.min_viable = 1;
    rule.tolerated_losses = n_plus;
    rule.name = "n-plus-" + std::to_string(n_plus);
    policy.redundancy.push_back(rule);
  }
  for (std::uint32_t i = 1; i <= 8; ++i) {
    DomainLimitRule rack;
    rack.domain = DomainId{DomainKind::Rack, i};
    rack.max_out = 1;
    rack.name = "one-per-rack";
    policy.domain_limits.push_back(rack);
    DomainLimitRule power;
    power.domain = DomainId{DomainKind::Power, i};
    power.max_out = 1;
    power.name = "one-per-feed";
    policy.domain_limits.push_back(power);
  }
  Contract contract;
  contract.id = ContractId::from_u64(1);
  contract.name = "web";
  contract.members = {TargetId{TargetKind::Link, 1}, TargetId{TargetKind::Link, 2}};
  contract.min_available = 1;
  policy.contracts.push_back(contract);

  policy.tiers = {PriorityTier{0, "emergency"}, PriorityTier{1, "routine"}};
  policy.min_evidence_sources = 1;
  policy.max_precondition_age = 60 * kNanosPerSecond;
  policy.default_duration = 10 * kNanosPerMinute;
  policy.max_concurrent_jobs = 8;
  policy.authority_lease = 10 * kNanosPerMinute;
  policy.drain_lease = 10 * kNanosPerMinute;
  policy.max_drain_failures_before_block = 3;
  policy.max_verification_attempts = 3;
  policy.max_restoration_attempts = 3;
  policy.evidence_ttl[EvidenceKind::TargetHealth] = 30 * kNanosPerSecond;
  policy.evidence_ttl[EvidenceKind::MaintenanceCompletion] = 10 * kNanosPerMinute;
  policy.evidence_ttl[EvidenceKind::DrainLeaseState] = 10 * kNanosPerMinute;
  policy.evidence_ttl[EvidenceKind::RestorationCheck] = 10 * kNanosPerMinute;
  return policy;
}

/// Strictly increasing revision source, so repeated observations of the same
/// key always advance rather than being rejected as replays.
inline std::uint64_t next_evidence_revision() {
  static std::atomic<std::uint64_t> counter{1};
  return counter.fetch_add(1);
}

inline mf::EvidenceRecord health_evidence(mf::TargetId target, mf::Health health, mf::Nanos now,
                                          mf::Nanos ttl, std::uint32_t source = 1) {
  using namespace mf;
  EvidenceRecord record;
  record.key.kind = EvidenceKind::TargetHealth;
  record.key.subject = EvidenceSubject::of(target);
  record.key.source = SourceId::from_u64(source);
  record.revision = Revision::from_u64(next_evidence_revision());
  record.id = EvidenceId::from_u64(record.revision.value());
  record.observed_at = now;
  record.ttl = ttl;
  record.klass = EvidenceClass::Volatile;
  record.payload.health = health;
  record.payload.result = true;
  return record;
}

/// Publishes healthy evidence for every target in the topology.
inline mf::Status seed_health(mf::Controller& controller, mf::Nanos now,
                              mf::Health health = mf::Health::Healthy) {
  for (const auto& [id, record] : controller.topology().targets()) {
    (void)record;
    const mf::Status observed = controller.observe(health_evidence(id, health, now,
                                                                  controller.policy().ttl_for(
                                                                      mf::EvidenceKind::TargetHealth)));
    if (!observed.ok()) {
      return observed;
    }
  }
  return mf::ok_status();
}

inline mf::Status seed_health_for(mf::Controller& controller,
                                  const std::vector<mf::TargetId>& targets, mf::Nanos now,
                                  mf::Health health = mf::Health::Healthy,
                                  std::uint32_t source = 1) {
  for (const mf::TargetId target : targets) {
    const mf::Status observed =
        controller.observe(health_evidence(target, health, now,
                                           controller.policy().ttl_for(
                                               mf::EvidenceKind::TargetHealth),
                                           source));
    if (!observed.ok()) {
      return observed;
    }
  }
  return mf::ok_status();
}

/// Opens a controller over a fresh journal with an in-process reference drain
/// service.
inline mf::Result<std::unique_ptr<mf::Controller>> open_controller(
    const std::string& journal_path, mf::ManualClock& clock, mf::DrainPortPtr drain,
    std::uint64_t nonce = 1) {
  mf::ControllerOptions options;
  options.store.journal_path = journal_path;
  options.nonce = nonce;
  options.name = "test";
  mf::RecoveryReport report;
  return mf::Controller::open(options, std::move(drain), clock, report);
}

/// Installs the standard fixture topology and policy.
inline mf::Status install_fixture(mf::Controller& controller, std::uint32_t n_plus = 1,
                                  std::size_t pools = 2) {
  const mf::Status topology = controller.set_topology(make_topology());
  if (!topology.ok()) {
    return topology;
  }
  return controller.set_policy(make_policy(n_plus, pools));
}

}  // namespace mftest
