// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/core/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "mf/core/limits.hpp"
#include "mf/core/time.hpp"

namespace mf {
namespace {

std::mutex& log_mutex() {
  static std::mutex mu;
  return mu;
}

std::atomic<LogLevel>& level_atomic() {
  static std::atomic<LogLevel> level{LogLevel::Warn};
  return level;
}

std::atomic<bool>& enabled_atomic() {
  static std::atomic<bool> enabled{true};
  return enabled;
}

std::atomic<std::uint64_t>& emitted_atomic() {
  static std::atomic<std::uint64_t> value{0};
  return value;
}

std::atomic<std::uint64_t>& suppressed_atomic() {
  static std::atomic<std::uint64_t> value{0};
  return value;
}

std::atomic<std::uint64_t>& truncated_atomic() {
  static std::atomic<std::uint64_t> value{0};
  return value;
}

}  // namespace

const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    case LogLevel::Trace: return "trace";
  }
  return "unknown";
}

bool parse_log_level(std::string_view text, LogLevel& out) noexcept {
  if (text == "error") { out = LogLevel::Error; return true; }
  if (text == "warn" || text == "warning") { out = LogLevel::Warn; return true; }
  if (text == "info") { out = LogLevel::Info; return true; }
  if (text == "debug") { out = LogLevel::Debug; return true; }
  if (text == "trace") { out = LogLevel::Trace; return true; }
  if (text == "off" || text == "none") { out = LogLevel::Error; return true; }
  return false;
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::set_level(LogLevel level) noexcept { level_atomic().store(level); }
LogLevel Logger::level() const noexcept { return level_atomic().load(); }
void Logger::set_enabled(bool enabled) noexcept { enabled_atomic().store(enabled); }
std::uint64_t Logger::emitted() const noexcept { return emitted_atomic().load(); }
std::uint64_t Logger::suppressed() const noexcept { return suppressed_atomic().load(); }
std::uint64_t Logger::truncated() const noexcept { return truncated_atomic().load(); }

void Logger::write(LogLevel level, std::string_view component, std::string_view message) {
  if (!enabled_atomic().load() || level > level_atomic().load()) {
    suppressed_atomic().fetch_add(1);
    return;
  }
  std::string body(message);
  if (body.size() > limits::kMaxLogMessageLength) {
    body.resize(limits::kMaxLogMessageLength);
    body += "...";
    truncated_atomic().fetch_add(1);
  }
  std::string line = format_time(SystemClock{}.now());
  line += " [";
  line += to_string(level);
  line += "] ";
  line += component;
  line += ": ";
  line += body;
  line.push_back('\n');
  std::lock_guard<std::mutex> guard(log_mutex());
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
  emitted_atomic().fetch_add(1);
}

void log_message(LogLevel level, std::string_view component, std::string_view message) {
  Logger::instance().write(level, component, message);
}

}  // namespace mf
