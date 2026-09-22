#pragma once

// Maintenance Fabric -- vendor-neutral maintenance eligibility and lifecycle.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string_view>

namespace mf {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Monotonic persisted/framed layout revision. Bumped whenever a journal
/// record layout or a transport frame layout changes incompatibly.
inline constexpr std::uint32_t kStateFormatVersion = 1;
inline constexpr std::uint32_t kProtocolVersion = 1;

inline constexpr std::string_view kProductName = "Maintenance Fabric";
inline constexpr std::string_view kProductVersion = "1.0.0";
inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs.";

}  // namespace mf
