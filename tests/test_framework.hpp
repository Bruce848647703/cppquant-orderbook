#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace testfw {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back(TestCase{std::move(name), std::move(fn)});
  }
};

class Failure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

template <typename T> std::string to_text(const T& value) {
  std::ostringstream out;
  if constexpr (std::is_same_v<T, bool>) {
    out << (value ? "true" : "false");
  } else if constexpr (std::is_enum_v<T>) {
    out << static_cast<long long>(value);
  } else {
    out << value;
  }
  return out.str();
}

template <typename T> std::string to_text(const std::vector<T>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    out << to_text(values[index]);
  }
  out << ']';
  return out.str();
}

template <typename T> std::string to_text(const std::optional<T>& value) {
  if (!value) {
    return "nullopt";
  }
  return "optional(" + to_text(*value) + ")";
}

inline std::string location(const char* file, int line) {
  return std::string(file) + ":" + std::to_string(line);
}

inline int run_all(const std::string& filter) {
  std::size_t passed = 0;
  std::size_t failed = 0;

  for (const auto& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    try {
      test.fn();
      std::cout << "[ PASS ] " << test.name << '\n';
      ++passed;
    } catch (const std::exception& error) {
      std::cout << "[ FAIL ] " << test.name << "\n         " << error.what()
                << '\n';
      ++failed;
    } catch (...) {
      std::cout << "[ FAIL ] " << test.name << "\n         unknown exception\n";
      ++failed;
    }
  }

  std::cout << '\n' << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

} // namespace testfw

#define TESTFW_CONCAT_INNER(a, b) a##b
#define TESTFW_CONCAT(a, b) TESTFW_CONCAT_INNER(a, b)

#define TEST(name)                                                             \
  static void name();                                                          \
  static ::testfw::Registrar TESTFW_CONCAT(testfw_reg_, name)(#name, name);    \
  static void name()

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw ::testfw::Failure(std::string("CHECK failed: ") + #condition +     \
                              " at " +                                         \
                              ::testfw::location(__FILE__, __LINE__));         \
    }                                                                          \
  } while (false)

#define CHECK_EQ(actual, expected)                                             \
  do {                                                                         \
    const auto& check_eq_a = (actual);                                         \
    const auto& check_eq_b = (expected);                                       \
    if (!(check_eq_a == check_eq_b)) {                                         \
      throw ::testfw::Failure(                                                 \
          std::string("CHECK_EQ failed: ") + #actual + " == " + #expected +    \
          "\n         actual:   " + ::testfw::to_text(check_eq_a) +            \
          "\n         expected: " + ::testfw::to_text(check_eq_b) +            \
          "\n         at " + ::testfw::location(__FILE__, __LINE__));          \
    }                                                                          \
  } while (false)

#define CHECK_NE(actual, unexpected)                                           \
  do {                                                                         \
    const auto& check_ne_a = (actual);                                         \
    const auto& check_ne_b = (unexpected);                                     \
    if (check_ne_a == check_ne_b) {                                            \
      throw ::testfw::Failure(std::string("CHECK_NE failed: ") + #actual +     \
                              " != " + #unexpected + " at " +                  \
                              ::testfw::location(__FILE__, __LINE__));         \
    }                                                                          \
  } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                \
  do {                                                                         \
    const long double check_near_a = static_cast<long double>(actual);         \
    const long double check_near_b = static_cast<long double>(expected);       \
    const long double check_near_t = static_cast<long double>(tolerance);      \
    if (std::fabs(check_near_a - check_near_b) > check_near_t) {               \
      throw ::testfw::Failure(                                                 \
          std::string("CHECK_NEAR failed: ") + #actual + " ~ " + #expected +   \
          "\n         actual:   " + ::testfw::to_text(check_near_a) +          \
          "\n         expected: " + ::testfw::to_text(check_near_b) +          \
          "\n         tolerance: " + ::testfw::to_text(check_near_t) +         \
          "\n         at " + ::testfw::location(__FILE__, __LINE__));          \
    }                                                                          \
  } while (false)

#define CHECK_THROW(expression, exception_type)                                \
  do {                                                                         \
    bool check_throw_caught = false;                                           \
    try {                                                                      \
      (void)(expression);                                                      \
    } catch (const exception_type&) {                                          \
      check_throw_caught = true;                                               \
    }                                                                          \
    if (!check_throw_caught) {                                                 \
      throw ::testfw::Failure(std::string("CHECK_THROW failed: ") +            \
                              #expression + " did not throw " +                \
                              #exception_type + " at " +                       \
                              ::testfw::location(__FILE__, __LINE__));         \
    }                                                                          \
  } while (false)
