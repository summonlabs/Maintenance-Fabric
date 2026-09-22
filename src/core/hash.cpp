// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/hash.hpp"

#include <array>
#include <cstdint>

namespace mf {
namespace {

/// CRC-32C (Castagnoli), reflected, polynomial 0x1EDC6F41.
constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256u; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1u) != 0u) {
        crc = (crc >> 1u) ^ 0x82F63B78u;
      } else {
        crc >>= 1u;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kCrcTable[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8u);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t h = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 1099511628211ull;
  }
  return h;
}

void Digest::update(const void* data, std::size_t size) noexcept {
  h_ = mix(h_, data, size);
}

std::uint64_t Digest::mix(std::uint64_t seed, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t h = seed;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 1099511628211ull;
  }
  return h;
}

void Digest::update_u64(std::uint64_t v) noexcept {
  unsigned char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<unsigned char>((v >> (8 * (7 - i))) & 0xFFull);
  }
  update(buf, sizeof(buf));
}

void Digest::update_u32(std::uint32_t v) noexcept {
  unsigned char buf[4];
  for (int i = 0; i < 4; ++i) {
    buf[i] = static_cast<unsigned char>((v >> (8 * (3 - i))) & 0xFFu);
  }
  update(buf, sizeof(buf));
}

void Digest::update_i64(std::int64_t v) noexcept {
  update_u64(static_cast<std::uint64_t>(v));
}

void Digest::update_bool(bool v) noexcept {
  const unsigned char b = v ? 1u : 0u;
  update(&b, 1);
}

std::string hex_u64(std::uint64_t value) {
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFull];
    value >>= 4u;
  }
  return out;
}

std::string hex_u32(std::uint32_t value) {
  std::string out(8, '0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFu];
    value >>= 4u;
  }
  return out;
}

}  // namespace mf
