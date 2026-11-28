// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_NO_IMPROVEMENT_STEPS_HPP
#define OPTIMARU_STOP_NO_IMPROVEMENT_STEPS_HPP

#include <cstdint>

#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// Stop once `limit` steps passed since the last new best
// (Limits::stall_step_limit; 0 = off).
class NoImprovementSteps {
public:
    explicit NoImprovementSteps(std::int64_t limit) : limit_(limit) {}

    bool reached(const StopState& s) const {
        return limit_ > 0 && s.step - s.last_improvement_step >= limit_;
    }

private:
    std::int64_t limit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_NO_IMPROVEMENT_STEPS_HPP
