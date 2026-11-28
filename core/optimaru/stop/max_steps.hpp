// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_MAX_STEPS_HPP
#define OPTIMARU_STOP_MAX_STEPS_HPP

#include <cstdint>

#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// Stop once `limit` steps were taken (Limits::step_limit; 0 = off).
class MaxSteps {
public:
    explicit MaxSteps(std::int64_t limit) : limit_(limit) {}

    bool reached(const StopState& s) const { return limit_ > 0 && s.step >= limit_; }

    // The fraction of the steps taken; 0 when off.
    double progress(const StopState& s) const {
        if (limit_ <= 0) return 0.0;
        return static_cast<double>(s.step) / static_cast<double>(limit_);
    }

private:
    std::int64_t limit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_MAX_STEPS_HPP
