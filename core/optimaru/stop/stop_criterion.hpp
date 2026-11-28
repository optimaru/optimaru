// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_STOP_CRITERION_HPP
#define OPTIMARU_STOP_STOP_CRITERION_HPP

#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// StopCriterion
// ---------------------------------------------------------------------------
// The plugin point for a stop rule of a use case's own (Solver::add_stop_criterion):
// asked once a step, after the built-in limits, with the state of the solve.
//
// state.now is the clock as last sampled, every 64 steps or after a long move
// (StopCondition::should_stop), so a time-based criterion fires up to 63 steps
// late; read the clock yourself only if that matters more than the cost of a
// clock read per step. A criterion that fires ends its own start of a
// multi-start run; the other starts go on to their own limits. progress() may
// be asked more than once per step: keep it free of side effects.
//
// The built-in criteria (one file each in this folder) are plain classes the
// StopCondition holds by value, so the per-step check stays free of virtual
// calls when nothing is registered.
class StopCriterion {
public:
    virtual ~StopCriterion() = default;
    // True once the solve should stop.
    virtual bool reached(const StopState& state) = 0;
    // How far the solve is towards this criterion, 0 -> 1, if it measures
    // that (simulated annealing cools and the history stages time by the
    // furthest criterion); 0 for one that does not.
    virtual double progress(const StopState& state) const {
        (void)state;
        return 0.0;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_STOP_CRITERION_HPP
