#include "pjs/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pjs/incremental_calculator.hpp"
#include "pjs/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

namespace pjs {

namespace {

// The domain Score of a plugin calculator, read from its three accumulators
// (level_scores(), before the medium fold) rather than decoded from the folded
// engine soft, which would file a soft total of 10^9 or more (a demoted or
// heavily re-weighted term) under medium.
Score levelScoreOf(const optimaru::DecomposedObjective<ProjectJobSchedule>& calculator) {
    const auto levels = calculator.level_scores();
    return {static_cast<std::int64_t>(levels.hard),
            optimaru::round_score_level(levels.medium, "medium"),
            optimaru::round_score_level(levels.soft, "soft")};
}

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the move mix, the ALNS knobs and the delay-guided value
// recommender (which reads only the working solution, so it applies to the
// plugin path too), driven by whichever calculator the caller brought.
// `reportedScore` turns the engine's new-best score into the domain Score
// handed to config.on_new_best, in the same levels as the final
// schedule.score of the calling overload.
void runEngine(const Solver::Config& config, ProjectJobSchedule& schedule,
               optimaru::Objective<ProjectJobSchedule>& calculator,
               std::function<Score(const optimaru::Score&)> reportedScore) {
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    if (config.on_new_best) {
        auto on_new_best = config.on_new_best;
        engineConfig.on_new_best = [on_new_best, reportedScore = std::move(reportedScore)](
                                           std::int64_t step, const optimaru::Score& score) {
            on_new_best(step, reportedScore(score));
        };
    }
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = toEngineScoreLimit(config.target_score);
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit = config.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    // 16x (up from the initial 8x) measured best once the allocation-free
    // calculator doubled moves/s: the longer horizon converts the extra
    // steps into deeper medium/soft improvements (x4 bench +31% at 16n vs
    // 8n; the clamp keeps demo-sized instances at the 400 floor).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(16 * schedule.allocations.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polish_steps = config.ruinRecreatePolishSteps;

    // Guided value selection (see Config::nearCurrentDelayProbability): a
    // recommended change-move value keeps the delay within a small geometric
    // delta of the entity's current delay — the delay-major composite
    // encoding makes uniform draws jump delays wildly, and almost all of
    // those jumps are wasted evaluations. A quarter of the guided draws keep
    // the delay unchanged so pure execution-mode switches stay frequent (at
    // a clamped delay 0 the +-delta draws degenerate to mode switches too,
    // which is where good schedules live). Returning -1 falls back to the
    // engine's uniform draw.
    if (config.nearCurrentDelayProbability > 0.0) {
        const double guidedProbability = config.nearCurrentDelayProbability;
        const ProjectJobSchedule* working = &schedule;
        engineConfig.search.value_hint = [working, guidedProbability](std::mt19937_64& rng,
                                                                      int allocationIndex) -> int {
            const int current = working->allocations[allocationIndex].modeDelay;
            if (current < 0) return -1;
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= guidedProbability) return -1;
            int delay = working->delayOfValue(current);
            if (unit(rng) >= 0.25) {
                // Geometric magnitude 1..8 (P(k) ~ 2^-k), random sign.
                int magnitude = 1;
                while ((rng() & 1u) != 0 && magnitude < 8) ++magnitude;
                delay += ((rng() & 1u) != 0 ? 1 : -1) * magnitude;
                delay = std::clamp(delay, 0, working->delayRangeSize - 1);
            }
            const int slot =
                    static_cast<int>(rng() % static_cast<std::uint64_t>(working->maxModeCount));
            return working->valueOf(delay, slot);
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    auto engine = optimaru::Solver<ProjectJobSchedule>(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    engine.solve(schedule, calculator);
}

}  // namespace

void Solver::solve(ProjectJobSchedule& schedule) {
    ScoreCalculator::prepare(schedule);  // validate + topological order
    // Pinned allocations must be initialized before solving (the engine
    // refuses an unassigned pinned entity): fix stragglers at
    // (first mode, delay 0).
    using Descriptor = optimaru::SolutionTraits<ProjectJobSchedule>;
    for (int a = 0; a < schedule.allocationCount(); ++a) {
        if (schedule.allocations[a].modeDelay < 0 && Descriptor::is_pinned(schedule, a)) {
            schedule.allocations[a].modeDelay = schedule.valueOf(0, 0);
        }
    }
    if (schedule.allocations.empty()) {
        ScoreCalculator::score(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }

    // Multi-start at the domain level instead of the engine's
    // solve_multistart: the guided value selection below captures the working
    // solution, so every start needs its own schedule copy AND its own
    // recommender wiring.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1) {
        std::vector<ProjectJobSchedule> candidates(workerCount, schedule);
        std::vector<std::exception_ptr> failures(workerCount);
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::run_worker_threads(workerCount, *stop,
                                     [this, &candidates, &failures, &stop](int i) {
                                         try {
                                             Config workerConfig = config_;
                                             workerConfig.parallelSolverCount = 1;
                                             workerConfig.stop_flag = stop;
                                             workerConfig.seed += static_cast<std::uint64_t>(i);
                                             if (i != 0) workerConfig.on_new_best = nullptr;
                                             Solver worker(std::move(workerConfig));
                                             worker.solve(candidates[i]);
                                         } catch (...) {
                                             failures[i] = std::current_exception();
                                             stop->store(true);
                                         }
                                     });
        for (const std::exception_ptr& failure : failures) {
            if (failure) std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i) {
            if (candidates[i].score > candidates[best].score) best = i;
        }
        schedule = std::move(candidates[best]);
        return;
    }

    ProjectJobSchedulingIncrementalScoreCalculator calculator;
    // The final score below is the monolithic recompute, whose integral
    // levels the fold decodes exactly (see domain.hpp), so each new best is
    // reported decoded from the engine score.
    runEngine(config_, schedule, calculator, fromEngineScore);

    // The authoritative final score (and derived dates): a full recompute on
    // the best solution.
    ScoreCalculator::score(schedule);
}

void Solver::solve(ProjectJobSchedule& schedule,
                   optimaru::DecomposedObjective<ProjectJobSchedule>& calculator) {
    ScoreCalculator::prepare(schedule);  // validate + topological order
    using Descriptor = optimaru::SolutionTraits<ProjectJobSchedule>;
    for (int a = 0; a < schedule.allocationCount(); ++a) {
        if (schedule.allocations[a].modeDelay < 0 && Descriptor::is_pinned(schedule, a)) {
            schedule.allocations[a].modeDelay = schedule.valueOf(0, 0);
        }
    }
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.allocations.empty()) {
        // The engine reports a new best right after the working solution
        // became it, so the calculator's accumulators hold exactly that
        // solution: report its levels the same way as the final
        // schedule.score below, not the decoded engine fold.
        runEngine(config_, schedule, calculator,
                  [&calculator](const optimaru::Score&) { return levelScoreOf(calculator); });
    }

    // Refresh the derived dates on the best solution (the plugin components
    // derive their own chain and never write these), then score it with the
    // caller's constraint assembly (custom weights and levels included),
    // leaving the calculator primed on it so constraint_totals() explains
    // exactly this score. The three levels come straight from the
    // calculator's accumulators (level_scores(), before the medium fold)
    // rather than from decoding the folded engine score, so a demoted or
    // re-weighted term lands on the level the assembly gave it.
    ScoreCalculator::updateDates(schedule);
    calculator.rebuild(schedule);
    schedule.score = levelScoreOf(calculator);
}

}  // namespace pjs
