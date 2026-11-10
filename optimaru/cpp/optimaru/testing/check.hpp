// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_TESTING_CHECK_HPP
#define OPTIMARU_TESTING_CHECK_HPP

#include <cstdio>
#include <string>

namespace optimaru::testing {

// The check() of the C++ test binaries: one PASS / FAIL line per expectation
// and a count of the failures, which main() turns into the exit code. A test
// binary is one program, so one counter serves all of its files.
inline int failures = 0;

inline void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

}  // namespace optimaru::testing

#endif  // OPTIMARU_TESTING_CHECK_HPP
