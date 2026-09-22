#pragma once

// Minimal dependency-free test harness. Tests run plainly: no watchdogs, no
// timeouts, no threads supervising test bodies. A hanging test is a defect.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <type_traits>
#include <typeinfo>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mftest {

struct Failure {
  std::string message;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
  bool disabled{false};
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline void fail(const std::string& message) { throw Failure{message}; }

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back(TestCase{suite, name, std::move(body), false});
  }
};

inline std::string location(const char* file, int line) {
  const std::string path(file);
  const std::size_t slash = path.find_last_of("/\\");
  std::ostringstream out;
  out << (slash == std::string::npos ? path : path.substr(slash + 1)) << ":" << line;
  return out.str();
}

template <class T, class = void>
struct has_stream_operator : std::false_type {};
template <class T>
struct has_stream_operator<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T, class = void>
struct has_adl_to_string : std::false_type {};
template <class T>
struct has_adl_to_string<T, std::void_t<decltype(to_string(std::declval<const T&>()))>>
    : std::true_type {};

inline std::string render(const std::string& value) { return "\"" + value + "\""; }
inline std::string render(const char* value) { return std::string("\"") + value + "\""; }
inline std::string render(bool value) { return value ? "true" : "false"; }

/// Renders a value for a failure message. Types without a stream operator fall
/// back to their ADL to_string, an enum's underlying value, or a placeholder, so
/// a comparison of strongly typed ids still produces a useful diagnostic.
template <class T>
std::string render(const T& value) {
  if constexpr (has_stream_operator<T>::value) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (has_adl_to_string<T>::value) {
    return to_string(value);
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return std::string("<") + typeid(T).name() + ">";
  }
}

inline int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--filter=", 0) == 0) {
      filter = arg.substr(9);
    } else if (arg == "--list") {
      list_only = true;
    }
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (list_only) {
      std::cout << full << "\n";
      continue;
    }
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    std::cout << "[ RUN  ] " << full << std::endl;
    try {
      test.body();
      ++passed;
      std::cout << "[  OK  ] " << full << std::endl;
    } catch (const Failure& failure) {
      ++failed;
      std::cout << "[ FAIL ] " << full << " -- " << failure.message << std::endl;
      failures.push_back(full + ": " + failure.message);
    } catch (const std::exception& ex) {
      ++failed;
      std::cout << "[ FAIL ] " << full << " -- unexpected exception: " << ex.what() << std::endl;
      failures.push_back(full + ": unexpected exception");
    }
  }
  if (list_only) {
    return 0;
  }
  std::cout << "\n=== " << passed << " passed, " << failed << " failed ===" << std::endl;
  for (const std::string& line : failures) {
    std::cout << "  " << line << std::endl;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace mftest

#define MF_TEST(suite, name)                                                        \
  static void mf_test_##suite##_##name();                                           \
  static ::mftest::Registrar mf_registrar_##suite##_##name(#suite, #name,           \
                                                          mf_test_##suite##_##name); \
  static void mf_test_##suite##_##name()

#define MF_FAIL(message)   ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": " + (message))

#define MF_CHECK(...)                                                           \
  do {                                                                          \
    if (!(__VA_ARGS__)) {                                                       \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) +                   \
                     ": CHECK failed: " #__VA_ARGS__);                          \
    }                                                                           \
  } while (false)

#define MF_CHECK_EQ(actual, expected)                                              \
  do {                                                                             \
    const auto& mf_a = (actual);                                                   \
    const auto& mf_b = (expected);                                                 \
    if (!(mf_a == mf_b)) {                                                         \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": CHECK_EQ failed: " #actual \
                     " == " #expected " (got " + ::mftest::render(mf_a) +           \
                     ", want " + ::mftest::render(mf_b) + ")");                    \
    }                                                                              \
  } while (false)

#define MF_CHECK_NE(actual, unexpected)                                            \
  do {                                                                             \
    const auto& mf_a = (actual);                                                   \
    const auto& mf_b = (unexpected);                                               \
    if (mf_a == mf_b) {                                                            \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": CHECK_NE failed: " #actual \
                     " != " #unexpected " (both " + ::mftest::render(mf_a) + ")");  \
    }                                                                              \
  } while (false)

#define MF_CHECK_OK(expr)                                                          \
  do {                                                                             \
    const auto& mf_status = (expr);                                                \
    if (!mf_status.ok()) {                                                         \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": expected success: " #expr \
                     " -- " + ::mf::format_error(mf_status.error()));              \
    }                                                                              \
  } while (false)

#define MF_CHECK_ERR(expr, expected_code)                                          \
  do {                                                                             \
    const auto& mf_status = (expr);                                                \
    if (mf_status.ok()) {                                                          \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": expected failure " #expected_code \
                     " but call succeeded: " #expr);                               \
    }                                                                              \
    if (mf_status.error().code != (expected_code)) {                                \
      ::mftest::fail(::mftest::location(__FILE__, __LINE__) + ": wrong error code for " #expr \
                     " -- got " + ::mf::format_error(mf_status.error()));          \
    }                                                                              \
  } while (false)

#define MF_TEST_MAIN()   int main(int argc, char** argv) { return ::mftest::run_all(argc, argv); }
