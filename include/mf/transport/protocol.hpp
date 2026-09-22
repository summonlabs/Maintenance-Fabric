#pragma once

// Application protocol carried inside transport frames: one opcode plus a
// compact, versioned body. Payload decoding is fully bounds checked and every
// declared count is validated before it is used.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/domain/drain.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/version.hpp"

namespace mf {

enum class OpCode : std::uint16_t {
  Ping = 1,
  Status = 2,
  List = 3,
  Propose = 4,
  Approve = 5,
  Rearm = 6,
  Cancel = 7,
  Tick = 8,
  Explain = 9,
  Conflicts = 10,
  ReportCompletion = 11,
  ReportRestoration = 12,
  Observe = 13,
  Quarantines = 14,
  ClearQuarantine = 15,
  DrainRequest = 100,
  DrainRelease = 101,
  DrainQuery = 102,
  DrainRefresh = 103,
};

[[nodiscard]] const char* to_string(OpCode op) noexcept;
[[nodiscard]] bool is_known_op(std::uint16_t value) noexcept;

/// Frame flag field carries the opcode; the body is length-prefixed text or a
/// typed record depending on the opcode.
inline constexpr std::uint32_t kStatusOk = 0;
inline constexpr std::uint32_t kStatusFailed = 1;

struct RpcEnvelope {
  std::uint16_t op{0};
  std::string message;
  std::vector<std::byte> body;
};

[[nodiscard]] std::vector<std::byte> encode_envelope(const RpcEnvelope& envelope);
[[nodiscard]] Result<RpcEnvelope> decode_envelope(std::span<const std::byte> bytes);

// --- typed bodies -----------------------------------------------------------

[[nodiscard]] std::vector<std::byte> encode_actor(std::string_view actor, std::string_view reason);
[[nodiscard]] Result<std::pair<std::string, std::string>> decode_actor(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_request_body(const MaintenanceRequest& request);
[[nodiscard]] Result<MaintenanceRequest> decode_request_body(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_job_ref(JobId job, std::string_view text);
[[nodiscard]] Result<std::pair<JobId, std::string>> decode_job_ref(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_snapshot(const JobSnapshot& snapshot);
[[nodiscard]] Result<JobSnapshot> decode_snapshot(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_snapshot_list(const std::vector<JobSnapshot>& snapshots);
[[nodiscard]] Result<std::vector<JobSnapshot>> decode_snapshot_list(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_job_actor(JobId job, std::string_view actor,
                                                        std::string_view reason);
[[nodiscard]] Result<std::tuple<JobId, std::string, std::string>> decode_job_actor(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_quarantine_clear(QuarantineId id, std::string_view actor,
                                                             std::string_view justification);
[[nodiscard]] Result<std::tuple<QuarantineId, std::string, std::string>> decode_quarantine_clear(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_completion_report(JobId job, const AttemptId& attempt,
                                                              bool succeeded,
                                                              std::string_view detail);
[[nodiscard]] Result<std::tuple<JobId, AttemptId, bool, std::string>> decode_completion_report(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_restoration_report(JobId job, const AttemptId& attempt,
                                                               TargetId target, bool ok,
                                                               std::string_view detail);
[[nodiscard]] Result<std::tuple<JobId, AttemptId, TargetId, bool, std::string>>
decode_restoration_report(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_evidence_body(const EvidenceRecord& evidence);
[[nodiscard]] Result<EvidenceRecord> decode_evidence_body(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_drain_request_body(const DrainRequest& request);
[[nodiscard]] Result<DrainRequest> decode_drain_request_body(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_drain_lease_body(const DrainLease& lease);
[[nodiscard]] Result<DrainLease> decode_drain_lease_body(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_drain_lease_ref(DrainLeaseId lease,
                                                            const ControllerIncarnation& requester,
                                                            Nanos extend);
[[nodiscard]] Result<std::tuple<DrainLeaseId, ControllerIncarnation, Nanos>> decode_drain_lease_ref(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_text_body(std::string_view text);
[[nodiscard]] Result<std::string> decode_text_body(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_i64_body(std::int64_t value);
[[nodiscard]] Result<std::int64_t> decode_i64_body(std::span<const std::byte> bytes);

}  // namespace mf
