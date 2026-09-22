#pragma once

// Bounded resource surfaces. Every externally derived size is clamped or
// rejected against these limits; nothing in the runtime allocates
// proportional to unvalidated input.

#include <cstddef>
#include <cstdint>

namespace mf::limits {

// --- Identifiers and names ---------------------------------------------------
inline constexpr std::size_t kMaxNameLength = 96;
inline constexpr std::size_t kMaxTitleLength = 160;
inline constexpr std::size_t kMaxReasonLength = 512;
inline constexpr std::size_t kMaxDetailLength = 1024;

// --- Maintenance requests ----------------------------------------------------
inline constexpr std::size_t kMaxTargetsPerJob = 4096;
inline constexpr std::size_t kMaxDependenciesPerJob = 64;
inline constexpr std::uint32_t kMaxAttemptsPerJob = 64;
inline constexpr std::uint32_t kMaxJobHistoryEntries = 1024;
inline constexpr std::uint64_t kMaxJobsTotal = 65536;
inline constexpr std::size_t kMaxJobsPerArbitration = 8192;

// --- Topology ----------------------------------------------------------------
inline constexpr std::size_t kMaxTargetsPerTopology = 65536;
inline constexpr std::size_t kMaxDomainsPerTopology = 16384;
inline constexpr std::size_t kMaxDomainsPerTarget = 64;
inline constexpr std::size_t kMaxPoolsPerTopology = 4096;
inline constexpr std::size_t kMaxMembersPerPool = 8192;
inline constexpr std::size_t kMaxTopologyChainDepth = 32;

// --- Policy ------------------------------------------------------------------
inline constexpr std::size_t kMaxRedundancyRules = 4096;
inline constexpr std::size_t kMaxDomainLimitRules = 4096;
inline constexpr std::size_t kMaxContracts = 4096;
inline constexpr std::size_t kMaxWindowRules = 1024;
inline constexpr std::size_t kMaxPriorityTiers = 16;
inline constexpr std::size_t kMaxWindowTargets = 4096;

// --- Evidence ----------------------------------------------------------------
inline constexpr std::size_t kMaxEvidenceRecords = 16384;
inline constexpr std::size_t kMaxEvidenceSourcesPerKey = 16;
inline constexpr std::int64_t kMaxEvidenceTtlNanos = 86400000000000LL;   // 24h
inline constexpr std::int64_t kMinEvidenceTtlNanos = 1000000000LL;       // 1s
inline constexpr std::int64_t kDefaultEvidenceTtlNanos = 30000000000LL;  // 30s
inline constexpr std::int64_t kMaxClockSkewNanos = 5000000000LL;         // 5s future tolerance

// --- Precondition authority --------------------------------------------------
inline constexpr std::int64_t kMaxPreconditionAgeNanos = 60000000000LL;  // 60s

// --- Drain fabric boundary ---------------------------------------------------
inline constexpr std::size_t kMaxDrainTargetsPerRequest = 512;
inline constexpr std::int64_t kMinDrainLeaseNanos = 1000000000LL;        // 1s
inline constexpr std::int64_t kMaxDrainLeaseNanos = 3600000000000LL;     // 1h
inline constexpr std::int64_t kDefaultDrainLeaseNanos = 600000000000LL;  // 10m
inline constexpr std::int64_t kMaxDrainLeaseSlackNanos = 5000000000LL;   // 5s

// --- Authority ledger --------------------------------------------------------
inline constexpr std::size_t kMaxLiveReservations = 4096;
inline constexpr std::int64_t kDefaultAuthorityLeaseNanos = 900000000000LL;  // 15m
inline constexpr std::int64_t kMaxAuthorityLeaseNanos = 7200000000000LL;     // 2h

// --- Persistence -------------------------------------------------------------
inline constexpr std::size_t kMaxJournalRecordBytes = 4u * 1024u * 1024u;
inline constexpr std::uint64_t kMaxJournalRecords = 4000000ull;
inline constexpr std::uint64_t kMaxJournalBytes = 512ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kJournalCompactionThresholdRecords = 8192u;
inline constexpr std::size_t kMaxSnapshotBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxRecordsPerTransaction = 512;

// --- Transport -----------------------------------------------------------------
inline constexpr std::size_t kMaxFrameBytes = 1u * 1024u * 1024u;
inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kMaxInFlightRequests = 1024;
inline constexpr std::uint32_t kMaxServerWorkers = 32;
inline constexpr std::uint32_t kMaxQueuedRequests = 4096;
inline constexpr std::int64_t kMaxRpcDeadlineNanos = 60000000000LL;
inline constexpr std::int64_t kDefaultRpcDeadlineNanos = 5000000000LL;

// --- Explanation / reporting ---------------------------------------------------
inline constexpr std::size_t kMaxExplanationSteps = 512;
inline constexpr std::size_t kMaxRejectedAlternatives = 64;
inline constexpr std::size_t kMaxConflictsReported = 256;
inline constexpr std::size_t kMaxLogMessageLength = 512;

}  // namespace mf::limits
