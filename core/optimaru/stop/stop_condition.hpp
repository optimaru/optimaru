// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_STOP_CONDITION_HPP
#define OPTIMARU_STOP_STOP_CONDITION_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/stop/max_runtime.hpp"
#include "optimaru/stop/max_steps.hpp"
#include "optimaru/stop/no_improvement_steps.hpp"
#include "optimaru/stop/no_improvement_time.hpp"
#include "optimaru/stop/stop_criterion.hpp"
#include "optimaru/stop/stop_requested.hpp"
#include "optimaru/stop/stop_state.hpp"
#include "optimaru/stop/target_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// StopCondition
// ---------------------------------------------------------------------------
// When a solve ends: the stop criteria of one solve, assembled from its
// Limits, asked once at the head of every search step. It stops the search as
// soon as ANY of them holds, in this order (one file each in this folder):
//
//   StopRequested       request_stop() was called on the solver, or a
//                       run-scoped stop flag was raised;
//   MaxRuntime          Limits::time_limit of wall time has passed (always set);
//   NoImprovementTime   Limits::stall_time_limit since the last new best (0 = off);
//   MaxSteps            Limits::step_limit steps were taken (0 = off);
//   NoImprovementSteps  Limits::stall_step_limit steps since the last new best
//                       (0 = off);
//   TargetScore         the best score reached Limits::target_score;
//
// then the criteria the use case added (StopCriterion, Solver::add_stop_criterion).
// The checks are plain comparisons on a handful of fields; the only costly
// part, reading the clock, is batched here (see should_stop).
class StopCondition {
public:
    explicit StopCondition(const Limits& limits, const std::atomic<bool>* stop_requested = nullptr,
                           const std::atomic<bool>* run_stop = nullptr,
                           std::vector<std::unique_ptr<StopCriterion>> custom = {})
        : requested_(stop_requested, run_stop),
          runtime_(limits.time_limit),
          stall_time_(limits.stall_time_limit),
          steps_(limits.step_limit),
          stall_steps_(limits.stall_step_limit),
          target_(limits.target_score),
          custom_(std::move(custom)) {
        state_.start = StopClock::now();
        state_.now = state_.start;
        state_.last_improvement = state_.start;
    }

    bool should_stop(std::int64_t step, const Score& best_score, bool force_time_check = false) {
        if (requested_.reached()) return true;
        // Wall-clock limits are expressed in milliseconds, while a cheap
        // incremental move can complete in tens of nanoseconds. Sampling a
        // platform clock after every such move consumed a material share of
        // the whole solve (about 16% on the school-timetabling benchmark).
        // Sample every 64 ordinary moves; callers force a sample after a
        // potentially expensive strategy/custom move. Step-based limits and
        // request_stop stay exact: they do not depend on the sample.
        if (force_time_check || (step & 63) == 0) state_.now = StopClock::now();
        state_.step = step;
        state_.best_score = best_score;
        if (runtime_.reached(state_) || stall_time_.reached(state_) || steps_.reached(state_) ||
            stall_steps_.reached(state_) || target_.reached(state_)) {
            return true;
        }
        for (const auto& criterion : custom_) {
            if (criterion->reached(state_)) return true;
        }
        return false;
    }

    // Call whenever the best score improves (feeds the stall limits).
    void on_improvement(std::int64_t step) {
        // Improvements are much rarer than move evaluations. Sample here so
        // a stall-time limit is measured from the exact improvement, even
        // when ordinary checks are batched.
        state_.last_improvement = StopClock::now();
        state_.last_improvement_step = step;
    }

    // 0 -> 1 over the run (feeds simulated annealing's cooling schedule and
    // the history stages): the furthest of the criteria that measure it, the
    // fraction of time_limit used and, when a step_limit is set, the fraction
    // of the steps taken. A step-limited run therefore cools over its steps
    // (and, whenever the step fraction leads, does so identically on every
    // run with the same seed) instead of staying near its starting
    // temperature because the wall-clock limit is far away.
    double progress() const {
        // should_stop() runs at the head of every step and already sampled
        // the clock into the state; reading it again here would add a second
        // clock read to every move.
        double g = std::max(runtime_.progress(state_), steps_.progress(state_));
        for (const auto& criterion : custom_) g = std::max(g, criterion->progress(state_));
        return g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
    }

private:
    StopRequested requested_;
    MaxRuntime runtime_;
    NoImprovementTime stall_time_;
    MaxSteps steps_;
    NoImprovementSteps stall_steps_;
    TargetScore target_;
    std::vector<std::unique_ptr<StopCriterion>> custom_;
    StopState state_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_STOP_CONDITION_HPP
