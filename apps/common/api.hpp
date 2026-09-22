#pragma once

// Transport-neutral command surface shared by the CLI's local (in-process) and
// remote (mfd over framed TCP) modes. Both implementations perform the same
// operations, so a CLI command behaves identically either way.

#include <memory>
#include <string>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/runtime/controller.hpp"
#include "mf/transport/client.hpp"
#include "mf/transport/protocol.hpp"

namespace mf::app {

class FabricApi {
 public:
  FabricApi() = default;
  virtual ~FabricApi();
  FabricApi(const FabricApi&) = delete;
  FabricApi& operator=(const FabricApi&) = delete;

  [[nodiscard]] virtual const char* mode() const noexcept = 0;
  [[nodiscard]] virtual Status load_topology(const std::string& path) = 0;
  [[nodiscard]] virtual Status load_policy(const std::string& path, Nanos now) = 0;

  [[nodiscard]] virtual Result<JobId> propose(const MaintenanceRequest& request,
                                              std::string actor) = 0;
  [[nodiscard]] virtual Status approve(JobId job, std::string actor) = 0;
  [[nodiscard]] virtual Status rearm(JobId job, std::string actor) = 0;
  [[nodiscard]] virtual Status cancel(JobId job, std::string actor, std::string reason) = 0;
  [[nodiscard]] virtual Status tick(Nanos now) = 0;

  [[nodiscard]] virtual Result<JobSnapshot> status(JobId job) = 0;
  [[nodiscard]] virtual Result<std::vector<JobSnapshot>> list() = 0;
  [[nodiscard]] virtual Result<std::string> explain(JobId job, std::string action) = 0;
  [[nodiscard]] virtual Result<std::string> conflicts() = 0;
  [[nodiscard]] virtual Result<std::string> arbitration() = 0;
  [[nodiscard]] virtual Result<std::string> quarantines() = 0;
  [[nodiscard]] virtual Status clear_quarantine(QuarantineId id, std::string actor,
                                                std::string justification) = 0;

  [[nodiscard]] virtual Status report_completion(JobId job, const AttemptId& attempt, bool ok,
                                                 std::string detail) = 0;
  [[nodiscard]] virtual Status report_restoration(JobId job, const AttemptId& attempt,
                                                  TargetId target, bool ok,
                                                  std::string detail) = 0;
  [[nodiscard]] virtual Status observe(const EvidenceRecord& evidence) = 0;

  [[nodiscard]] virtual Result<std::string> topology_text() = 0;
  [[nodiscard]] virtual Result<std::string> policy_text() = 0;
};

/// In-process implementation backed by a Controller.
class LocalApi final : public FabricApi {
 public:
  explicit LocalApi(std::unique_ptr<Controller> controller);

  [[nodiscard]] static Result<std::unique_ptr<LocalApi>> open(const ControllerOptions& options,
                                                              DrainPortPtr drain,
                                                              const Clock& clock,
                                                              RecoveryReport& report);

  [[nodiscard]] Controller& controller() noexcept { return *controller_; }
  [[nodiscard]] const Controller& controller() const noexcept { return *controller_; }

  [[nodiscard]] const char* mode() const noexcept override { return "local"; }
  [[nodiscard]] Status load_topology(const std::string& path) override;
  [[nodiscard]] Status load_policy(const std::string& path, Nanos now) override;
  [[nodiscard]] Result<JobId> propose(const MaintenanceRequest& request,
                                      std::string actor) override;
  [[nodiscard]] Status approve(JobId job, std::string actor) override;
  [[nodiscard]] Status rearm(JobId job, std::string actor) override;
  [[nodiscard]] Status cancel(JobId job, std::string actor, std::string reason) override;
  [[nodiscard]] Status tick(Nanos now) override;
  [[nodiscard]] Result<JobSnapshot> status(JobId job) override;
  [[nodiscard]] Result<std::vector<JobSnapshot>> list() override;
  [[nodiscard]] Result<std::string> explain(JobId job, std::string action) override;
  [[nodiscard]] Result<std::string> conflicts() override;
  [[nodiscard]] Result<std::string> arbitration() override;
  [[nodiscard]] Result<std::string> quarantines() override;
  [[nodiscard]] Status clear_quarantine(QuarantineId id, std::string actor,
                                        std::string justification) override;
  [[nodiscard]] Status report_completion(JobId job, const AttemptId& attempt, bool ok,
                                         std::string detail) override;
  [[nodiscard]] Status report_restoration(JobId job, const AttemptId& attempt, TargetId target,
                                          bool ok, std::string detail) override;
  [[nodiscard]] Status observe(const EvidenceRecord& evidence) override;
  [[nodiscard]] Result<std::string> topology_text() override;
  [[nodiscard]] Result<std::string> policy_text() override;

 private:
  std::unique_ptr<Controller> controller_;
};

/// Remote implementation talking to mfd over the framed transport.
class RemoteApi final : public FabricApi {
 public:
  RemoteApi() = default;

  [[nodiscard]] static Result<std::unique_ptr<RemoteApi>> connect(const std::string& host,
                                                                  std::uint16_t port,
                                                                  Nanos deadline);

  [[nodiscard]] const char* mode() const noexcept override { return "remote"; }
  [[nodiscard]] Status load_topology(const std::string& path) override;
  [[nodiscard]] Status load_policy(const std::string& path, Nanos now) override;
  [[nodiscard]] Result<JobId> propose(const MaintenanceRequest& request,
                                      std::string actor) override;
  [[nodiscard]] Status approve(JobId job, std::string actor) override;
  [[nodiscard]] Status rearm(JobId job, std::string actor) override;
  [[nodiscard]] Status cancel(JobId job, std::string actor, std::string reason) override;
  [[nodiscard]] Status tick(Nanos now) override;
  [[nodiscard]] Result<JobSnapshot> status(JobId job) override;
  [[nodiscard]] Result<std::vector<JobSnapshot>> list() override;
  [[nodiscard]] Result<std::string> explain(JobId job, std::string action) override;
  [[nodiscard]] Result<std::string> conflicts() override;
  [[nodiscard]] Result<std::string> arbitration() override;
  [[nodiscard]] Result<std::string> quarantines() override;
  [[nodiscard]] Status clear_quarantine(QuarantineId id, std::string actor,
                                        std::string justification) override;
  [[nodiscard]] Status report_completion(JobId job, const AttemptId& attempt, bool ok,
                                         std::string detail) override;
  [[nodiscard]] Status report_restoration(JobId job, const AttemptId& attempt, TargetId target,
                                          bool ok, std::string detail) override;
  [[nodiscard]] Status observe(const EvidenceRecord& evidence) override;
  [[nodiscard]] Result<std::string> topology_text() override;
  [[nodiscard]] Result<std::string> policy_text() override;

 private:
  [[nodiscard]] Result<std::vector<std::byte>> call(OpCode op,
                                                    const std::vector<std::byte>& body,
                                                    std::string_view what);
  [[nodiscard]] Status call_status(OpCode op, const std::vector<std::byte>& body,
                                   std::string_view what);

  std::unique_ptr<TcpClient> client_;
  Nanos deadline_{limits::kDefaultRpcDeadlineNanos};
};

}  // namespace mf::app
