#pragma once

// Bounded structured logging to stderr. No telemetry is ever transmitted; the
// runtime writes only to the process's own standard error stream.

#include <cstdint>
#include <string>
#include <string_view>

namespace mf {

enum class LogLevel : std::uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

[[nodiscard]] const char* to_string(LogLevel level) noexcept;
[[nodiscard]] bool parse_log_level(std::string_view text, LogLevel& out) noexcept;

class Logger {
 public:
  static Logger& instance();

  void set_level(LogLevel level) noexcept;
  [[nodiscard]] LogLevel level() const noexcept;
  void set_enabled(bool enabled) noexcept;

  void write(LogLevel level, std::string_view component, std::string_view message);

  [[nodiscard]] std::uint64_t emitted() const noexcept;
  [[nodiscard]] std::uint64_t suppressed() const noexcept;
  [[nodiscard]] std::uint64_t truncated() const noexcept;

 private:
  Logger() = default;
};

void log_message(LogLevel level, std::string_view component, std::string_view message);

#define MF_LOG(level, component, message)   ::mf::log_message(::mf::LogLevel::level, (component), (message))

}  // namespace mf
