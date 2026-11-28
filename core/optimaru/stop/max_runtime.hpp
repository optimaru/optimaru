// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_MAX_RUNTIME_HPP
#define OPTIMARU_STOP_MAX_RUNTIME_HPP

#include <chrono>

#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// Stop once `limit` of wall time has passed since the solve started
// (Limits::time_limit, always set).
class MaxRuntime {
public:
    explicit MaxRuntime(std::chrono::milliseconds limit) : limit_(limit) {}

    bool reached(const StopState& s) const { return s.now - s.start >= limit_; }

    // The fraction of the limit used; 1 for a limit of 0.
    double progress(const StopState& s) const {
        const double total = std::chrono::duration<double>(limit_).count();
        if (total <= 0) return 1.0;
        return std::chrono::duration<double>(s.now - s.start).count() / total;
    }

private:
    std::chrono::milliseconds limit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_MAX_RUNTIME_HPP
