// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/transport/frame.hpp"

#include <cstring>

#include "mf/core/bytes.hpp"
#include "mf/core/hash.hpp"
#include "mf/core/limits.hpp"

namespace mf {
namespace {

constexpr std::uint16_t kMagic = 0x4D46u;  // "MF"

void write_u32(unsigned char* p, std::uint32_t value) {
  p[0] = static_cast<unsigned char>((value >> 24u) & 0xFFu);
  p[1] = static_cast<unsigned char>((value >> 16u) & 0xFFu);
  p[2] = static_cast<unsigned char>((value >> 8u) & 0xFFu);
  p[3] = static_cast<unsigned char>(value & 0xFFu);
}

std::uint32_t read_u32(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
         (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}

std::uint16_t read_u16(const unsigned char* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8u) |
                                    static_cast<std::uint16_t>(p[1]));
}

std::uint64_t read_u64(const unsigned char* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8u) | static_cast<std::uint64_t>(p[i]);
  }
  return value;
}

void write_u64(unsigned char* p, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>((value >> (8 * (7 - i))) & 0xFFull);
  }
}

}  // namespace

const char* to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::Unknown: return "unknown";
    case FrameType::Hello: return "hello";
    case FrameType::HelloAck: return "hello-ack";
    case FrameType::Request: return "request";
    case FrameType::Response: return "response";
    case FrameType::Error: return "error";
    case FrameType::Heartbeat: return "heartbeat";
    case FrameType::Goodbye: return "goodbye";
  }
  return "unknown";
}

bool is_known_frame_type(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(FrameType::Hello) &&
         value <= static_cast<std::uint16_t>(FrameType::Goodbye);
}

Status validate_frame(const Frame& frame) {
  if (!is_known_frame_type(static_cast<std::uint16_t>(frame.type))) {
    return invalid_argument("frame type is not recognised");
  }
  if (frame.version != kProtocolVersion) {
    return make_error(ErrorCode::VersionMismatch,
                      "frame protocol revision " + std::to_string(frame.version) +
                          " is not supported (expected " + std::to_string(kProtocolVersion) + ")");
  }
  if (frame.payload.size() > limits::kMaxFrameBytes) {
    return make_error(ErrorCode::LimitExceeded, "frame payload exceeds the configured bound");
  }
  return ok_status();
}

std::vector<std::byte> encode_frame(const Frame& frame) {
  std::vector<std::byte> out(kFrameHeaderBytes + frame.payload.size());
  auto* bytes = reinterpret_cast<unsigned char*>(out.data());
  std::memset(bytes, 0, kFrameHeaderBytes);
  bytes[0] = static_cast<unsigned char>((kMagic >> 8u) & 0xFFu);
  bytes[1] = static_cast<unsigned char>(kMagic & 0xFFu);
  bytes[2] = static_cast<unsigned char>((frame.version >> 8u) & 0xFFu);
  bytes[3] = static_cast<unsigned char>(frame.version & 0xFFu);
  bytes[4] = static_cast<unsigned char>((static_cast<std::uint16_t>(frame.type) >> 8u) & 0xFFu);
  bytes[5] = static_cast<unsigned char>(static_cast<std::uint16_t>(frame.type) & 0xFFu);
  write_u32(bytes + 8, frame.flags);
  write_u64(bytes + 12, frame.request_id);
  write_u32(bytes + 20, static_cast<std::uint32_t>(frame.payload.size()));
  write_u32(bytes + 24, crc32c(frame.payload.data(), frame.payload.size()));
  write_u32(bytes + 28, crc32c(bytes, 28));
  if (!frame.payload.empty()) {
    std::memcpy(bytes + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  return out;
}

Result<FrameHeader> decode_frame_header(std::span<const std::byte> bytes) {
  if (bytes.size() < kFrameHeaderBytes) {
    return make_error(ErrorCode::CorruptState, "frame header is truncated");
  }
  const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
  if (read_u16(raw) != kMagic) {
    return make_error(ErrorCode::CorruptState, "frame magic does not match Maintenance Fabric");
  }
  if (crc32c(raw, 28) != read_u32(raw + 28)) {
    return make_error(ErrorCode::CorruptState, "frame header checksum mismatch");
  }
  FrameHeader header;
  header.version = read_u16(raw + 2);
  const std::uint16_t type = read_u16(raw + 4);
  if (!is_known_frame_type(type)) {
    return make_error(ErrorCode::CorruptState, "frame declares an unrecognised type");
  }
  header.type = static_cast<FrameType>(type);
  header.flags = read_u32(raw + 8);
  header.request_id = read_u64(raw + 12);
  header.payload_length = read_u32(raw + 20);
  if (header.payload_length > limits::kMaxFrameBytes) {
    return make_error(ErrorCode::LimitExceeded, "frame declares a payload beyond the bound");
  }
  if (header.version != kProtocolVersion) {
    return make_error(ErrorCode::VersionMismatch,
                      "frame protocol revision " + std::to_string(header.version) +
                          " is not supported");
  }
  return header;
}

Result<Frame> decode_frame(std::span<const std::byte> bytes) {
  Result<FrameHeader> header = decode_frame_header(bytes);
  if (!header.ok()) {
    return header.error();
  }
  if (bytes.size() != kFrameHeaderBytes + header.value().payload_length) {
    return make_error(ErrorCode::CorruptState, "frame length does not match its header");
  }
  const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
  if (crc32c(raw + kFrameHeaderBytes, header.value().payload_length) != read_u32(raw + 24)) {
    return make_error(ErrorCode::CorruptState, "frame payload checksum mismatch");
  }
  Frame frame;
  frame.version = header.value().version;
  frame.type = header.value().type;
  frame.flags = header.value().flags;
  frame.request_id = header.value().request_id;
  frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                       bytes.end());
  return frame;
}

}  // namespace mf
