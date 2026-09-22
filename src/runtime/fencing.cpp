// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/runtime/fencing.hpp"

namespace mf {

Status require_current_incarnation(const ControllerIncarnation& recorded,
                                   const ControllerIncarnation& current, std::string_view what) {
  if (recorded == current) {
    return ok_status();
  }
  return make_error(ErrorCode::StaleIncarnation,
                    std::string(what) + " carries incarnation " + to_string(recorded) +
                        " but the running incarnation is " + to_string(current));
}

Status require_attempt(const AttemptId& recorded, const AttemptId& expected, std::string_view what) {
  if (recorded == expected) {
    return ok_status();
  }
  return make_error(ErrorCode::StaleAttempt,
                    std::string(what) + " carries attempt " + to_string(recorded) +
                        " but the current attempt is " + to_string(expected));
}

Status require_generation(GenerationId recorded, GenerationId expected, std::string_view what) {
  if (recorded == expected) {
    return ok_status();
  }
  return make_error(ErrorCode::StaleGeneration,
                    std::string(what) + " carries generation " + to_string(recorded) +
                        " but the current generation is " + to_string(expected));
}

Status require_revision(Revision recorded, Revision expected, std::string_view what) {
  if (recorded == expected) {
    return ok_status();
  }
  return make_error(ErrorCode::StaleRevision,
                    std::string(what) + " carries revision " + to_string(recorded) +
                        " but the current revision is " + to_string(expected));
}

}  // namespace mf
