#pragma once

// Strongly typed identities. Domain objects are never identified by a bare
// integer or an interchangeable string: two ids of different kinds do not
// compare, convert, or mix.

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace mf {

/// Parses an unsigned decimal integer with explicit overflow rejection.
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept;

template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_u64(std::uint64_t value) noexcept {
    return StrongId(value);
  }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr StrongId next() const noexcept { return StrongId(value_ + 1); }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

 private:
  std::uint64_t value_{0};
};

template <class Tag>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(StrongId<Tag> id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

/// Per-tag textual prefix used by the CLI, the journal and explanations.
template <class Tag>
struct IdTraits;

template <class Tag>
[[nodiscard]] std::string to_string(StrongId<Tag> id) {
  std::string out(IdTraits<Tag>::kPrefix);
  out.push_back(':');
  out += std::to_string(id.value());
  return out;
}

template <class Tag>
[[nodiscard]] std::optional<StrongId<Tag>> parse_id(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  if (text.substr(0, colon) != std::string_view(IdTraits<Tag>::kPrefix)) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> value = parse_u64(text.substr(colon + 1));
  if (!value.has_value()) {
    return std::nullopt;
  }
  return StrongId<Tag>::from_u64(*value);
}

// --- Concrete identity types -------------------------------------------------

struct JobTag;
struct GenerationTag;
struct AttemptTag;
struct EvidenceTag;
struct SourceTag;
struct AuthorityTag;
struct ControllerTag;
struct BootEpochTag;
struct RevisionTag;
struct SequenceTag;
struct DrainLeaseTag;
struct PoolTag;
struct ContractTag;
struct WindowTag;
struct RequestorTag;
struct QuarantineTag;

using JobId = StrongId<JobTag>;
using GenerationId = StrongId<GenerationTag>;
using AttemptOrdinal = StrongId<AttemptTag>;
using EvidenceId = StrongId<EvidenceTag>;
using SourceId = StrongId<SourceTag>;
using AuthorityId = StrongId<AuthorityTag>;
using ControllerId = StrongId<ControllerTag>;
using BootEpoch = StrongId<BootEpochTag>;
using Revision = StrongId<RevisionTag>;
using RecordSeq = StrongId<SequenceTag>;
using DrainLeaseId = StrongId<DrainLeaseTag>;
using PoolId = StrongId<PoolTag>;
using ContractId = StrongId<ContractTag>;
using WindowId = StrongId<WindowTag>;
using RequestorId = StrongId<RequestorTag>;
using QuarantineId = StrongId<QuarantineTag>;

#define MF_DEFINE_ID_TRAITS(TagType, PrefixLiteral, NameLiteral)   \
  template <>                                                      \
  struct IdTraits<TagType> {                                       \
    static constexpr std::string_view kPrefix = PrefixLiteral;      \
    static constexpr std::string_view kName = NameLiteral;          \
  }

MF_DEFINE_ID_TRAITS(JobTag, "job", "job");
MF_DEFINE_ID_TRAITS(GenerationTag, "gen", "generation");
MF_DEFINE_ID_TRAITS(AttemptTag, "att", "attempt");
MF_DEFINE_ID_TRAITS(EvidenceTag, "ev", "evidence");
MF_DEFINE_ID_TRAITS(SourceTag, "src", "source");
MF_DEFINE_ID_TRAITS(AuthorityTag, "auth", "authority");
MF_DEFINE_ID_TRAITS(ControllerTag, "ctl", "controller");
MF_DEFINE_ID_TRAITS(BootEpochTag, "boot", "boot-epoch");
MF_DEFINE_ID_TRAITS(RevisionTag, "rev", "revision");
MF_DEFINE_ID_TRAITS(SequenceTag, "seq", "sequence");
MF_DEFINE_ID_TRAITS(DrainLeaseTag, "lease", "drain-lease");
MF_DEFINE_ID_TRAITS(PoolTag, "pool", "pool");
MF_DEFINE_ID_TRAITS(ContractTag, "ctr", "contract");
MF_DEFINE_ID_TRAITS(WindowTag, "win", "window");
MF_DEFINE_ID_TRAITS(RequestorTag, "req", "requestor");
MF_DEFINE_ID_TRAITS(QuarantineTag, "quar", "quarantine");

#undef MF_DEFINE_ID_TRAITS

// --- Composite attempt identity ---------------------------------------------

/// Identity of one execution attempt of one generation of one job. The triple
/// is the fencing token for every externally reported maintenance outcome:
/// an attempt that does not match the job's current generation and live
/// attempt ordinal can never complete the job.
struct AttemptId {
  JobId job;
  GenerationId generation;
  AttemptOrdinal ordinal;

  friend constexpr bool operator==(const AttemptId&, const AttemptId&) noexcept = default;
  friend constexpr auto operator<=>(const AttemptId&, const AttemptId&) noexcept = default;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return job.valid() && generation.valid() && ordinal.valid();
  }
};

[[nodiscard]] std::string to_string(const AttemptId& id);
[[nodiscard]] std::optional<AttemptId> parse_attempt_id(std::string_view text);

/// Stable, non-zero requestor identity derived from an operator or service name.
[[nodiscard]] RequestorId requestor_from_name(std::string_view name) noexcept;

}  // namespace mf
