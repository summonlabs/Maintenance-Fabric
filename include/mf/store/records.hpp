#pragma once

// Versioned, integrity-checked record encodings. Every payload starts with its
// own layout revision so a future runtime can refuse a record it does not
// understand instead of misinterpreting it. Decoding is fully bounds checked:
// a declared count never causes an allocation larger than the record itself.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mf/core/bytes.hpp"
#include "mf/core/result.hpp"
#include "mf/domain/authority.hpp"
#include "mf/domain/drain.hpp"
#include "mf/domain/evidence.hpp"
#include "mf/domain/job.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"

namespace mf {

enum class RecordType : std::uint16_t {
  Unknown = 0,
  Transaction = 1,
  Topology = 2,
  Policy = 3,
  Job = 4,
  Evidence = 5,
  Reservation = 6,
  Lease = 7,
  Quarantine = 8,
  Boot = 9,
  JobErased = 10,
  ReservationErased = 11,
  LeaseErased = 12,
  QuarantineErased = 13,
  Marker = 14,
};

[[nodiscard]] const char* to_string(RecordType type) noexcept;
[[nodiscard]] bool is_known_record_type(std::uint16_t value) noexcept;

struct Record {
  RecordType type{RecordType::Unknown};
  std::vector<std::byte> payload;
};

/// Rejects records larger than limits::kMaxJournalRecordBytes.
[[nodiscard]] Status validate_record(const Record& record);

[[nodiscard]] Record encode_topology_record(const Topology& topology);
[[nodiscard]] Status decode_topology_record(const Record& record, Topology& topology);

[[nodiscard]] Record encode_policy_record(const Policy& policy);
[[nodiscard]] Status decode_policy_record(const Record& record, Policy& policy);

[[nodiscard]] Record encode_job_record(const MaintenanceJob& job);
[[nodiscard]] Status decode_job_record(const Record& record, MaintenanceJob& job);
[[nodiscard]] Record encode_job_erased_record(JobId job);
[[nodiscard]] Status decode_job_erased_record(const Record& record, JobId& job);

[[nodiscard]] Record encode_evidence_record(const EvidenceRecord& evidence);
[[nodiscard]] Status decode_evidence_record(const Record& record, EvidenceRecord& evidence);

[[nodiscard]] Record encode_reservation_record(const AuthorityReservation& reservation);
[[nodiscard]] Status decode_reservation_record(const Record& record,
                                               AuthorityReservation& reservation);
[[nodiscard]] Record encode_reservation_erased_record(AuthorityId id);
[[nodiscard]] Status decode_reservation_erased_record(const Record& record, AuthorityId& id);

[[nodiscard]] Record encode_lease_record(const DrainLease& lease);
[[nodiscard]] Status decode_lease_record(const Record& record, DrainLease& lease);
[[nodiscard]] Record encode_lease_erased_record(DrainLeaseId id);
[[nodiscard]] Status decode_lease_erased_record(const Record& record, DrainLeaseId& id);

[[nodiscard]] Record encode_quarantine_record(const QuarantineRecord& record);
[[nodiscard]] Status decode_quarantine_record(const Record& record, QuarantineRecord& out);

[[nodiscard]] Record encode_boot_record(const ControllerIncarnation& incarnation,
                                        BootEpoch prior_epoch);
[[nodiscard]] Status decode_boot_record(const Record& record, ControllerIncarnation& incarnation,
                                        BootEpoch& prior_epoch);

[[nodiscard]] Record encode_marker_record(std::uint64_t value);

/// One atomic unit of persistence. Replay applies every sub-record or none.
[[nodiscard]] Record encode_transaction_record(const std::vector<Record>& records);
[[nodiscard]] Status decode_transaction_record(const Record& record, std::vector<Record>& out);

}  // namespace mf
