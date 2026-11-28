// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_NO_IMPROVEMENT_TIME_HPP
#define OPTIMARU_STOP_NO_IMPROVEMENT_TIME_HPP

#include <chrono>

#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// Stop once `limit` of wall time has passed since the last new best
// (Limits::stall_time_limit; 0 = off).
class NoImprovementTime {
public:
    explicit NoImprovementTime(std::chrono::milliseconds limit) : limit_(limit) {}

    bool reached(const StopState& s) const {
        return limit_.count() > 0 && s.now - s.last_improvement >= limit_;
    }

private:
    std::chrono::milliseconds limit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_NO_IMPROVEMENT_TIME_HPP
