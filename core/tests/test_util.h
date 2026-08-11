// SPDX-License-Identifier: GPL-3.0-only
//
// A deliberately tiny assertion harness. The engine has no test-framework
// dependency so `cmake && ctest` works on a bare CI runner and inside the NDK
// build without fetching anything.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace openipc::test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void ReportFailure(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
}

inline int Summary(const char* suite) {
    std::fprintf(stderr, "%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace openipc::test

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        ++::openipc::test::g_checks;                                                      \
        if (!(condition)) {                                                               \
            ::openipc::test::ReportFailure(__FILE__, __LINE__, "CHECK(" #condition ")");   \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        ++::openipc::test::g_checks;                                                      \
        const auto actual_value = (actual);                                               \
        const auto expected_value = (expected);                                           \
        if (!(actual_value == expected_value)) {                                          \
            ::openipc::test::ReportFailure(                                               \
                __FILE__, __LINE__,                                                       \
                std::string(#actual " == " #expected " (got ") +                          \
                    std::to_string(actual_value) + ", want " +                            \
                    std::to_string(expected_value) + ")");                                \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                           \
    do {                                                                                  \
        ++::openipc::test::g_checks;                                                      \
        const double actual_value = static_cast<double>(actual);                          \
        const double expected_value = static_cast<double>(expected);                      \
        if (std::fabs(actual_value - expected_value) > (tolerance)) {                     \
            ::openipc::test::ReportFailure(                                               \
                __FILE__, __LINE__,                                                       \
                std::string(#actual " ~= " #expected " (got ") +                          \
                    std::to_string(actual_value) + ", want " +                            \
                    std::to_string(expected_value) + ")");                                \
        }                                                                                 \
    } while (0)

#define CHECK_STREQ(actual, expected)                                                     \
    do {                                                                                  \
        ++::openipc::test::g_checks;                                                      \
        const std::string actual_value = (actual);                                        \
        const std::string expected_value = (expected);                                    \
        if (actual_value != expected_value) {                                             \
            ::openipc::test::ReportFailure(__FILE__, __LINE__,                            \
                                           std::string(#actual " (got \"") +              \
                                               actual_value + "\", want \"" +             \
                                               expected_value + "\")");                   \
        }                                                                                 \
    } while (0)
