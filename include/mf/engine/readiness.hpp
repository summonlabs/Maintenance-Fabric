#pragma once

// A readiness view is the only way the engine reads operational health. It is
// built exclusively from evidence that is fresh under the current incarnation,
// so a decision can never be made from a deserialized observation that merely
// survived a restart.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mf/domain/evidence.hpp"
#include "mf/domain/policy.hpp"
#include "mf/domain/topology.hpp"

namespace mf {

struct TargetReadiness {
  TargetId target;
  Health health{Health::Unknown};
  bool fresh{false};
  std::size_t fresh_sources{0};
  SourceId source;
  Nanos observed_at{0};
  FreshnessState state{FreshnessState::Missing};
  std::string detail;
};

class ReadinessView {
 public:
  ReadinessView() = default;

  [[nodiscard]] static ReadinessView build(const Topology& topology,
                                           const EvidenceStore& evidence, const Policy& policy,
                                           const ControllerIncarnation& current, Nanos now);

  [[nodiscard]] const TargetReadiness* get(TargetId id) const noexcept;
  [[nodiscard]] Health health_of(TargetId id) const noexcept;
  [[nodiscard]] bool is_fresh(TargetId id) const noexcept;
  [[nodiscard]] bool is_serving(TargetId id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::map<TargetId, TargetReadiness>& entries() const noexcept {
    return entries_;
  }
  /// Targets in p scope whose readiness is not fresh, ascending.
  [[nodiscard]] std::vector<TargetId> unfresh(const std::vector<TargetId>& scope) const;
  [[nodiscard]] Nanos built_at() const noexcept { return built_at_; }
  [[nodiscard]] std::uint64_t digest() const;

 private:
  std::map<TargetId, TargetReadiness> entries_;
  Nanos built_at_{0};
};

}  // namespace mf
