// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/bytes.hpp"

#include <cstring>
#include <limits>

#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"

namespace mf {

ByteWriter::ByteWriter(std::size_t reserve_bytes) {
  if (reserve_bytes > 0 && reserve_bytes <= limits::kMaxSnapshotBytes) {
    buffer_.reserve(reserve_bytes);
  }
}

void ByteWriter::u8(std::uint8_t value) {
  buffer_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  u16(static_cast<std::uint16_t>((value >> 16u) & 0xFFFFu));
  u16(static_cast<std::uint16_t>(value & 0xFFFFu));
}

void ByteWriter::u64(std::uint64_t value) {
  u32(static_cast<std::uint32_t>((value >> 32u) & 0xFFFFFFFFu));
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

void ByteWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::boolean(bool value) {
  u8(value ? 1u : 0u);
}

void ByteWriter::raw(const void* data, std::size_t size) {
  if (size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::byte*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void ByteWriter::raw(std::span<const std::byte> data) {
  raw(data.data(), data.size());
}

void ByteWriter::str(std::string_view value, std::size_t max_length) {
  if (value.size() > max_length || value.size() > std::numeric_limits<std::uint16_t>::max()) {
    overflowed_ = true;
    u16(0);
    return;
  }
  u16(static_cast<std::uint16_t>(value.size()));
  raw(value.data(), value.size());
}

void ByteWriter::blob(std::span<const std::byte> data, std::size_t max_length) {
  if (data.size() > max_length || data.size() > std::numeric_limits<std::uint32_t>::max()) {
    overflowed_ = true;
    u32(0);
    return;
  }
  u32(static_cast<std::uint32_t>(data.size()));
  raw(data);
}

void ByteWriter::patch_u32(std::size_t offset, std::uint32_t value) {
  if (offset + 4u > buffer_.size()) {
    overflowed_ = true;
    return;
  }
  buffer_[offset + 0u] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  buffer_[offset + 1u] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  buffer_[offset + 2u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  buffer_[offset + 3u] = static_cast<std::byte>(value & 0xFFu);
}

bool ByteReader::need(std::size_t count) noexcept {
  if (!ok_) {
    return false;
  }
  if (count > data_.size() - offset_) {
    ok_ = false;
    return false;
  }
  return true;
}

std::uint8_t ByteReader::u8() noexcept {
  if (!need(1)) {
    return 0;
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  offset_ += 1;
  return value;
}

std::uint16_t ByteReader::u16() noexcept {
  const std::uint16_t hi = u8();
  const std::uint16_t lo = u8();
  return static_cast<std::uint16_t>((hi << 8u) | lo);
}

std::uint32_t ByteReader::u32() noexcept {
  const std::uint32_t hi = u16();
  const std::uint32_t lo = u16();
  return (hi << 16u) | lo;
}

std::uint64_t ByteReader::u64() noexcept {
  const std::uint64_t hi = u32();
  const std::uint64_t lo = u32();
  return (hi << 32u) | lo;
}

std::int64_t ByteReader::i64() noexcept {
  return static_cast<std::int64_t>(u64());
}

bool ByteReader::boolean() noexcept { return u8() != 0u; }

void ByteReader::raw(void* out, std::size_t size) noexcept {
  if (size == 0) {
    return;
  }
  if (!need(size)) {
    std::memset(out, 0, size);
    return;
  }
  std::memcpy(out, data_.data() + offset_, size);
  offset_ += size;
}

std::string ByteReader::str(std::size_t max_length) {
  const std::uint16_t length = u16();
  if (!ok_ || static_cast<std::size_t>(length) > max_length) {
    ok_ = false;
    return {};
  }
  if (!need(length)) {
    return {};
  }
  std::string out(reinterpret_cast<const char*>(data_.data() + offset_), length);
  offset_ += length;
  return out;
}

std::span<const std::byte> ByteReader::blob(std::size_t max_length) {
  const std::uint32_t length = u32();
  if (!ok_ || static_cast<std::size_t>(length) > max_length) {
    ok_ = false;
    return {};
  }
  if (!need(length)) {
    return {};
  }
  const std::span<const std::byte> out = data_.subspan(offset_, length);
  offset_ += length;
  return out;
}

}  // namespace mf
