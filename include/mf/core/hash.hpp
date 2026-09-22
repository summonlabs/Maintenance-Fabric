#pragma once

// Deterministic, dependency-free hashing used for integrity checks and stable
// ordering. CRC32-C protects journal records and wire frames; FNV-1a 64 builds
// content digests for explanations and decision records.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mf {

[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view s) noexcept {
  return crc32c(s.data(), s.size());
}

[[nodiscard]] std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s) noexcept {
  return fnv1a64(s.data(), s.size());
}

/// Incremental digest used when building explanations and decision records.
/// Feeding the same values in the same order always yields the same value.
class Digest {
 public:
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view s) noexcept { update(s.data(), s.size()); }
  void update_u64(std::uint64_t v) noexcept;
  void update_u32(std::uint32_t v) noexcept;
  void update_i64(std::int64_t v) noexcept;
  void update_bool(bool v) noexcept;
  [[nodiscard]] std::uint64_t value() const noexcept { return h_; }

 private:
  [[nodiscard]] static std::uint64_t mix(std::uint64_t seed, const void* data,
                                         std::size_t size) noexcept;

  std::uint64_t h_{14695981039346656037ull};
};

/// Hex helpers for digests printed by the CLI.
[[nodiscard]] std::string hex_u64(std::uint64_t value);
[[nodiscard]] std::string hex_u32(std::uint32_t value);

}  // namespace mf
