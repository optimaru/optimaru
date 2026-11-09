// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine, a modified derivative of Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_SOLVER_TERMINATION_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_SOLVER_TERMINATION_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/config/solver/termination/termination_config.hpp"
#include "optimaru/impl/solver/termination/best_score.hpp"
#include "optimaru/impl/solver/termination/step_count.hpp"
#include "optimaru/impl/solver/termination/termination_criterion.hpp"
#include "optimaru/impl/solver/termination/time_millis_spent.hpp"
#include "optimaru/impl/solver/termination/unimproved_step_count.hpp"
#include "optimaru/impl/solver/termination/unimproved_time_millis_spent.hpp"

namespace optimaru {

// The solve-level composite (upstream's OrCompositeTermination +
// SolverTermination): builds the child criteria from TerminationConfig, owns
// the shared clock/improvement state, and terminates when ANY child does.
//
// A criterion is only wired when its config field asks for it, so the
// per-check cost stays proportional to what the caller actually configured.
class SolverTermination {
public:
    // terminateEarlyFlag: Solver::terminateEarly(). stopFlag: a second,
    // run-scoped stop (a multi-start run raises it when one start fails, so
    // its siblings end at their next step).
    explicit SolverTermination(const TerminationConfig& config,
                               const std::atomic<bool>* terminateEarlyFlag = nullptr,
                               const std::atomic<bool>* stopFlag = nullptr)
        : spentLimit_(config.spentLimit),
          stepCountLimit_(config.stepCountLimit),
          terminateEarlyFlag_(terminateEarlyFlag),
          stopFlag_(stopFlag) {
        children_.push_back(std::make_unique<TimeMillisSpentTermination>(config.spentLimit));
        if (config.unimprovedSpentLimit.count() > 0) {
            children_.push_back(std::make_unique<UnimprovedTimeMillisSpentTermination>(
                    config.unimprovedSpentLimit));
        }
        if (config.stepCountLimit > 0) {
            children_.push_back(std::make_unique<StepCountTermination>(config.stepCountLimit));
        }
        if (config.unimprovedStepCountLimit > 0) {
            children_.push_back(std::make_unique<UnimprovedStepCountTermination>(
                    config.unimprovedStepCountLimit));
        }
        children_.push_back(std::make_unique<BestScoreTermination>(config.bestScoreLimit));
        context_.start = Clock::now();
        context_.lastImprovement = context_.start;
    }

    bool isTerminated(std::int64_t step, const HardSoftScore& bestScore,
                      bool forceTimeCheck = false) {
        if (terminateEarlyFlag_ && terminateEarlyFlag_->load(std::memory_order_relaxed)) {
            return true;  // Solver::terminateEarly() (used by SolverManager)
        }
        if (stopFlag_ && stopFlag_->load(std::memory_order_relaxed)) return true;
        // Wall-clock limits are expressed in milliseconds, while a cheap
        // incremental move can complete in tens of nanoseconds. Sampling a
        // platform clock after every such move consumed a material share of
        // the whole solve (about 16% on the school-timetabling benchmark).
        // Check every 64 ordinary moves; callers force a sample after a
        // potentially expensive strategy/custom move. Step-based limits and
        // terminateEarly remain exact because all criteria still run here.
        if (forceTimeCheck || (step & 63) == 0) context_.now = Clock::now();
        context_.step = step;
        context_.bestScore = bestScore;
        for (const auto& child : children_) {
            if (child->isTerminated(context_)) return true;
        }
        return false;
    }

    // Call whenever the best score improves (feeds the unimproved limits).
    void onImprovement(std::int64_t step) {
        // Improvements are much rarer than move evaluations. Sample here so
        // an unimproved-time limit is measured from the exact improvement,
        // even when ordinary termination checks are batched.
        context_.lastImprovement = Clock::now();
        context_.lastImprovementStep = step;
    }

    // 0 -> 1 over the run (feeds simulated annealing's cooling schedule): the
    // fraction of spentLimit used, or, when a stepCountLimit is set, the
    // fraction of the steps taken if that is further along. A step-limited
    // run therefore cools over its steps (and, whenever the step fraction
    // leads, does so identically on every run with the same seed) instead
    // of staying near its starting temperature because the wall-clock
    // limit is far away.
    double timeGradient() const {
        // isTerminated() is called once at the head of every local-search
        // step and already sampled the clock into context_.now. Acceptor
        // arguments are evaluated even for the default Late Acceptance
        // acceptor, which ignores the gradient, so reading the clock again
        // here used to add a second syscall to every move in every domain.
        double elapsed =
                std::chrono::duration<double>(context_.now - context_.start).count();
        double total = std::chrono::duration<double>(spentLimit_).count();
        if (total <= 0) return 1.0;
        double g = elapsed / total;
        if (stepCountLimit_ > 0) {
            g = std::max(g, static_cast<double>(context_.step) /
                                    static_cast<double>(stepCountLimit_));
        }
        return g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
    }

private:
    using Clock = std::chrono::steady_clock;
    std::chrono::milliseconds spentLimit_;
    std::int64_t stepCountLimit_;
    const std::atomic<bool>* terminateEarlyFlag_;
    const std::atomic<bool>* stopFlag_;
    std::vector<std::unique_ptr<Termination>> children_;
    TerminationContext context_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_SOLVER_TERMINATION_HPP
