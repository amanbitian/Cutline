#pragma once

// A deliberately small test harness.
//
// The project has no third-party test framework yet, and the value of one here
// would be marginal: what these tests need is named cases, an assertion that
// reports the failing expression and line, and a non-zero exit code. Adding
// GoogleTest can wait until there are fixtures worth sharing.

#include <chrono>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif
#include <iostream>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::testing {

// Reads an environment variable. MSVC deprecates getenv in favour of a
// bounds-checked variant, so this wraps both rather than silencing the warning.
[[nodiscard]] inline std::string EnvironmentValue(const char* name) {
#ifdef _MSC_VER
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

struct Case final {
  std::string name;
  std::function<void()> body;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Failure final : std::exception {
  std::string message;
  explicit Failure(std::string text) : message(std::move(text)) {}
  [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

// Thrown by SKIP_UNLESS. A skipped test is reported distinctly from a passing
// one: a test that quietly passes because its fixture was missing is worse than
// no test, because it reads as coverage that does not exist.
struct Skipped final : std::exception {
  std::string reason;
  // True when the test's premise does not hold here (an audio device that is not
  // present, a refusal that cannot happen on a build that can encode), as opposed to
  // a prerequisite that should have been there and was not (a media fixture, FFmpeg).
  // CUTLINE_STRICT turns the second kind into failures; the first kind stays skipped.
  bool inapplicable{false};
  explicit Skipped(std::string text, bool does_not_apply = false)
      : reason(std::move(text)), inapplicable(does_not_apply) {}
  [[nodiscard]] const char* what() const noexcept override { return reason.c_str(); }
};

inline void Register(std::string name, std::function<void()> body) {
  Registry().push_back({std::move(name), std::move(body)});
}

[[noreturn]] inline void Fail(std::string_view expression, std::string_view file, int line,
                              std::string_view detail = {}) {
  std::string message = std::string(file) + ":" + std::to_string(line) + ": " + std::string(expression);
  if (!detail.empty()) message += " -- " + std::string(detail);
  throw Failure(message);
}

// Runs every registered case. Returns a process exit code.
inline int RunAll(std::string_view suite) {
  int failures = 0;
  int skipped = 0;
#ifdef _MSC_VER
  // A failed debug-runtime assertion (a vector index out of range) should end the run with a message and a non-zero exit,
  // not wait behind a dialog box for someone to press a button.
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  // CUTLINE_ONLY=text runs only the cases whose name contains the text; CUTLINE_TRACE=1 names each case before it runs, so a
  // crash says where.
  const auto only = EnvironmentValue("CUTLINE_ONLY");
  const bool trace = !EnvironmentValue("CUTLINE_TRACE").empty();
  const auto strict_setting = EnvironmentValue("CUTLINE_STRICT");
  const bool strict = !strict_setting.empty() && strict_setting != "0";
  for (const auto& test : Registry()) {
    if (!only.empty() && std::string_view(test.name).find(only) == std::string_view::npos) continue;
    if (trace) std::cerr << "  running " << test.name << std::endl;
    const auto started = std::chrono::steady_clock::now();
    try {
      test.body();
      const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      std::cout << "  ok   " << test.name;
      if (seconds >= 2.0) std::cout << "  (" << static_cast<int>(seconds) << " s)";
      std::cout << "\n";
    } catch (const Skipped& skip) {
      if (strict && !skip.inapplicable) {
        std::cout << "  FAIL " << test.name << "\n       required prerequisite missing (CUTLINE_STRICT): " << skip.what()
                  << "\n";
        ++failures;
        continue;
      }
      std::cout << "  skip " << test.name << " (" << skip.what() << ")\n";
      ++skipped;
    } catch (const Failure& failure) {
      std::cout << "  FAIL " << test.name << "\n       " << failure.what() << "\n";
      ++failures;
    } catch (const std::exception& error) {
      std::cout << "  FAIL " << test.name << "\n       unexpected exception: " << error.what() << "\n";
      ++failures;
    }
  }
  const auto total = Registry().size();
  const auto passed = total - static_cast<std::size_t>(failures) - static_cast<std::size_t>(skipped);
  std::cout << suite << ": " << passed << "/" << total << " passed";
  if (skipped != 0) std::cout << ", " << skipped << " skipped";
  std::cout << "\n";
  return failures == 0 ? 0 : 1;
}

}  // namespace cutline::testing

// Defines and registers a test case.
#define CUTLINE_TEST(name)                                                             \
  static void name();                                                                  \
  namespace {                                                                          \
  const struct name##_Registrar final {                                                \
    name##_Registrar() { cutline::testing::Register(#name, []() { name(); }); }         \
  } name##_registrar_instance;                                                          \
  }                                                                                    \
  static void name()

// Abandons the test as skipped. Used where a test needs something the build may
// not have -- generated media fixtures, an audio device -- so the absence is
// reported rather than silently reducing coverage.
#define SKIP_UNLESS(condition, reason)                                                 \
  do {                                                                                 \
    if (!(condition)) throw cutline::testing::Skipped(reason);                          \
  } while (false)

// Abandons the test as not applicable: its premise does not hold on this build or
// machine. Unlike SKIP_UNLESS this stays a skip under CUTLINE_STRICT.
#define SKIP_INAPPLICABLE(condition, reason)                                           \
  do {                                                                                 \
    if (!(condition)) throw cutline::testing::Skipped(reason, true);                    \
  } while (false)

#define CHECK(condition)                                                               \
  do {                                                                                 \
    if (!(condition)) cutline::testing::Fail("CHECK(" #condition ")", __FILE__, __LINE__); \
  } while (false)

#define CHECK_EQ(left, right)                                                          \
  do {                                                                                 \
    const auto& check_left = (left);                                                    \
    const auto& check_right = (right);                                                  \
    if (!(check_left == check_right)) {                                                 \
      cutline::testing::Fail("CHECK_EQ(" #left ", " #right ")", __FILE__, __LINE__,      \
                             ToDiagnostic(check_left) + " vs " + ToDiagnostic(check_right)); \
    }                                                                                   \
  } while (false)

// Asserts that `statement` throws. The expression is evaluated only for its
// exceptions, so its result -- including a [[nodiscard]] one -- is discarded.
// Asserts that `statement` throws. Used heavily: most of what a project store
// does is refuse invalid edits, and a test that only covers the happy path would
// not notice if validation were removed.
#define CHECK_THROWS(statement)                                                        \
  do {                                                                                 \
    bool threw = false;                                                                \
    try {                                                                              \
      (void)(statement);                                                               \
    } catch (const std::exception&) {                                                   \
      threw = true;                                                                     \
    }                                                                                   \
    if (!threw) cutline::testing::Fail("CHECK_THROWS(" #statement ")", __FILE__, __LINE__); \
  } while (false)

#define CHECK_NO_THROW(statement)                                                      \
  do {                                                                                 \
    try {                                                                              \
      (void)(statement);                                                               \
    } catch (const std::exception& error) {                                             \
      cutline::testing::Fail("CHECK_NO_THROW(" #statement ")", __FILE__, __LINE__, error.what()); \
    }                                                                                   \
  } while (false)

// Overloads used by CHECK_EQ to describe a mismatch.
inline std::string ToDiagnostic(const std::string& value) { return "\"" + value + "\""; }
inline std::string ToDiagnostic(const char* value) { return std::string("\"") + value + "\""; }
inline std::string ToDiagnostic(bool value) { return value ? "true" : "false"; }
template <typename T>
inline std::string ToDiagnostic(const T& value) {
  return std::to_string(value);
}
