#pragma once

// Framed wire protocol. Every frame carries a layout revision, an explicit
// length, a payload checksum and a header checksum; a decoder rejects a frame
// whose declared length exceeds the configured maximum before allocating.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/version.hpp"

namespace mf {

enum class FrameType : std::uint16_t {
  Unknown = 0,
  Hello = 1,
  HelloAck = 2,
  Request = 3,
  Response = 4,
  Error = 5,
  Heartbeat = 6,
  Goodbye = 7,
};

[[nodiscard]] const char* to_string(FrameType type) noexcept;
[[nodiscard]] bool is_known_frame_type(std::uint16_t value) noexcept;

inline constexpr std::size_t kFrameHeaderBytes = 32;

struct Frame {
  std::uint16_t version{kProtocolVersion};
  FrameType type{FrameType::Unknown};
  std::uint32_t flags{0};
  std::uint64_t request_id{0};
  std::vector<std::byte> payload;
};

[[nodiscard]] Status validate_frame(const Frame& frame);
[[nodiscard]] std::vector<std::byte> encode_frame(const Frame& frame);
[[nodiscard]] Result<Frame> decode_frame(std::span<const std::byte> bytes);
/// Decodes a frame header only, so a reader can size its next read safely.
struct FrameHeader {
  std::uint16_t version{0};
  FrameType type{FrameType::Unknown};
  std::uint32_t flags{0};
  std::uint64_t request_id{0};
  std::uint32_t payload_length{0};
};
[[nodiscard]] Result<FrameHeader> decode_frame_header(std::span<const std::byte> bytes);

}  // namespace mf
