// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_STOP_CONDITION_HPP
#define OPTIMARU_STOP_STOP_CONDITION_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// When a solve ends. One object per solve, asked once at the head of every
// search step; it stops the search as soon as ANY of these holds:
//
//   - request_stop() was called on the solver, or a run-scoped stop flag was
//     raised (a multi-start run raises it when one start fails or reaches
//     the target);
//   - Limits::time_limit of wall time has passed (always set);
//   - Limits::stall_time_limit has passed since the last new best (0 = off);
//   - Limits::step_limit steps were taken (0 = off);
//   - Limits::stall_step_limit steps passed since the last new best (0 = off);
//   - the best score reached Limits::target_score.
//
// The checks are plain comparisons on a handful of fields; the only costly
// part, reading the clock, is batched (see should_stop).
class StopCondition {
public:
    explicit StopCondition(const Limits& limits, const std::atomic<bool>* stop_requested = nullptr,
                           const std::atomic<bool>* run_stop = nullptr)
        : time_limit_(limits.time_limit),
          stall_time_limit_(limits.stall_time_limit),
          step_limit_(limits.step_limit),
          stall_step_limit_(limits.stall_step_limit),
          target_score_(limits.target_score),
          stop_requested_(stop_requested),
          run_stop_(run_stop),
          start_(Clock::now()),
          now_(start_),
          last_improvement_(start_) {}

    bool should_stop(std::int64_t step, const Score& best_score, bool force_time_check = false) {
        if (stop_requested_ && stop_requested_->load(std::memory_order_relaxed)) return true;
        if (run_stop_ && run_stop_->load(std::memory_order_relaxed)) return true;
        // Wall-clock limits are expressed in milliseconds, while a cheap
        // incremental move can complete in tens of nanoseconds. Sampling a
        // platform clock after every such move consumed a material share of
        // the whole solve (about 16% on the school-timetabling benchmark).
        // Sample every 64 ordinary moves; callers force a sample after a
        // potentially expensive strategy/custom move. Step-based limits and
        // request_stop stay exact: they do not depend on the sample.
        if (force_time_check || (step & 63) == 0) now_ = Clock::now();
        step_ = step;
        if (now_ - start_ >= time_limit_) return true;
        if (stall_time_limit_.count() > 0 && now_ - last_improvement_ >= stall_time_limit_) {
            return true;
        }
        if (step_limit_ > 0 && step >= step_limit_) return true;
        if (stall_step_limit_ > 0 && step - last_improvement_step_ >= stall_step_limit_) {
            return true;
        }
        return best_score >= target_score_;
    }

    // Call whenever the best score improves (feeds the stall limits).
    void on_improvement(std::int64_t step) {
        // Improvements are much rarer than move evaluations. Sample here so
        // a stall-time limit is measured from the exact improvement, even
        // when ordinary checks are batched.
        last_improvement_ = Clock::now();
        last_improvement_step_ = step;
    }

    // 0 -> 1 over the run (feeds simulated annealing's cooling schedule): the
    // fraction of time_limit used, or, when a step_limit is set, the
    // fraction of the steps taken if that is further along. A step-limited
    // run therefore cools over its steps (and, whenever the step fraction
    // leads, does so identically on every run with the same seed) instead
    // of staying near its starting temperature because the wall-clock
    // limit is far away.
    double progress() const {
        // should_stop() runs at the head of every step and already sampled
        // the clock into now_; reading it again here would add a second
        // clock read to every move.
        const double elapsed = std::chrono::duration<double>(now_ - start_).count();
        const double total = std::chrono::duration<double>(time_limit_).count();
        if (total <= 0) return 1.0;
        double g = elapsed / total;
        if (step_limit_ > 0) {
            g = std::max(g, static_cast<double>(step_) / static_cast<double>(step_limit_));
        }
        return g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
    }

private:
    using Clock = std::chrono::steady_clock;

    std::chrono::milliseconds time_limit_;
    std::chrono::milliseconds stall_time_limit_;
    std::int64_t step_limit_;
    std::int64_t stall_step_limit_;
    Score target_score_;
    const std::atomic<bool>* stop_requested_;
    const std::atomic<bool>* run_stop_;

    Clock::time_point start_;
    Clock::time_point now_;
    Clock::time_point last_improvement_;
    std::int64_t step_ = 0;
    std::int64_t last_improvement_step_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_STOP_CONDITION_HPP
