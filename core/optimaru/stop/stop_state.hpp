// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_STOP_STATE_HPP
#define OPTIMARU_STOP_STOP_STATE_HPP

#include <chrono>
#include <cstdint>

#include "optimaru/score/score.hpp"

namespace optimaru {

// What every stop criterion reads: where the solve stands at the head of a
// step. StopCondition keeps it current; the clock in `now` is sampled in
// batches (StopCondition::should_stop), never read by a criterion itself.
using StopClock = std::chrono::steady_clock;

struct StopState {
    std::int64_t step = 0;
    Score best_score;
    StopClock::time_point start;
    StopClock::time_point now;
    StopClock::time_point last_improvement;
    std::int64_t last_improvement_step = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_STOP_STATE_HPP
