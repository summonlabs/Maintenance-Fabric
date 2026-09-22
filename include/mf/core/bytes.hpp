#pragma once

// Bounds-checked big-endian binary encoding used by the journal and the wire
// protocol. Readers never read past their buffer and never allocate based on an
// unvalidated length field.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mf {

class ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(std::size_t reserve_bytes);

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void raw(const void* data, std::size_t size);
  void raw(std::span<const std::byte> data);
  /// Length-prefixed (u16) string. Rejects strings longer than max_length.
  void str(std::string_view value, std::size_t max_length);
  /// Length-prefixed (u32) byte blob, bounded by max_length.
  void blob(std::span<const std::byte> data, std::size_t max_length);

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::span<const std::byte> view() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }

  /// Patch a previously reserved u32 slot (used for frame headers).
  void patch_u32(std::size_t offset, std::uint32_t value);

 private:
  std::vector<std::byte> buffer_;
  bool overflowed_{false};
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t u8() noexcept;
  [[nodiscard]] std::uint16_t u16() noexcept;
  [[nodiscard]] std::uint32_t u32() noexcept;
  [[nodiscard]] std::uint64_t u64() noexcept;
  [[nodiscard]] std::int64_t i64() noexcept;
  [[nodiscard]] bool boolean() noexcept;
  void raw(void* out, std::size_t size) noexcept;
  [[nodiscard]] std::string str(std::size_t max_length);
  [[nodiscard]] std::span<const std::byte> blob(std::size_t max_length);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool at_end() const noexcept { return ok_ && offset_ == data_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return ok_ ? data_.size() - offset_ : 0;
  }

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;

  std::span<const std::byte> data_;
  std::size_t offset_{0};
  bool ok_{true};
};

}  // namespace mf
