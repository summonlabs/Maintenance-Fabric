// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/result.hpp"

#include <string>

namespace mf {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::AlreadyExists: return "already-exists";
    case ErrorCode::LimitExceeded: return "limit-exceeded";
    case ErrorCode::Overflow: return "overflow";
    case ErrorCode::StaleGeneration: return "stale-generation";
    case ErrorCode::StaleIncarnation: return "stale-incarnation";
    case ErrorCode::StaleRevision: return "stale-revision";
    case ErrorCode::StaleEvidence: return "stale-evidence";
    case ErrorCode::StaleAttempt: return "stale-attempt";
    case ErrorCode::StaleAuthority: return "stale-authority";
    case ErrorCode::PolicyDenied: return "policy-denied";
    case ErrorCode::StateConflict: return "state-conflict";
    case ErrorCode::WindowClosed: return "window-closed";
    case ErrorCode::DependencyUnmet: return "dependency-unmet";
    case ErrorCode::EvidenceMissing: return "evidence-missing";
    case ErrorCode::DrainFailed: return "drain-failed";
    case ErrorCode::DrainLeaseLost: return "drain-lease-lost";
    case ErrorCode::RestorationFailed: return "restoration-failed";
    case ErrorCode::Unauthorized: return "unauthorized";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ShuttingDown: return "shutting-down";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::IoError: return "io-error";
    case ErrorCode::CorruptState: return "corrupt-state";
    case ErrorCode::VersionMismatch: return "version-mismatch";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

Error make_error(ErrorCode code, std::string detail) {
  Error error;
  error.code = code;
  error.detail = std::move(detail);
  return error;
}

std::string format_error(const Error& error) {
  std::string out = to_string(error.code);
  if (!error.detail.empty()) {
    out += ": ";
    out += error.detail;
  }
  return out;
}

}  // namespace mf
