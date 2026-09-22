#pragma once

// Fence checks. Every path that mutates current state on behalf of an external
// actor proves first that the actor's token is still the current one.

#include <string>
#include <string_view>

#include "mf/core/id.hpp"
#include "mf/core/result.hpp"
#include "mf/domain/identity.hpp"

namespace mf {

/// Identity + revision pair carried by any deferred decision.
struct FenceToken {
  ControllerIncarnation incarnation;
  Revision revision;
};

[[nodiscard]] Status require_current_incarnation(const ControllerIncarnation& recorded,
                                                 const ControllerIncarnation& current,
                                                 std::string_view what);
[[nodiscard]] Status require_attempt(const AttemptId& recorded, const AttemptId& expected,
                                     std::string_view what);
[[nodiscard]] Status require_generation(GenerationId recorded, GenerationId expected,
                                        std::string_view what);
[[nodiscard]] Status require_revision(Revision recorded, Revision expected, std::string_view what);

}  // namespace mf
