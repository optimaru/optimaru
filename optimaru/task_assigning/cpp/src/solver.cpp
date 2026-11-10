#include "ta/solver.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ta/incremental_calculator.hpp"
#include "ta/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace ta {

namespace {

// Translates a domain Config into the engine's Settings: termination,
// late acceptance, the sequence move mix and the ALNS knobs. Shared by both
// solve() overloads.
optimaru::Settings buildEngineConfig(const Solver::Config& config,
                                     const TaskAssigningSolution& solution, int parallel_starts) {
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.parallel_starts = parallel_starts;
    if (config.on_new_best) {
        auto on_new_best = config.on_new_best;
        engineConfig.on_new_best = [on_new_best](std::int64_t step, const optimaru::Score& score) {
            on_new_best(step, fromEngineScore(score));
        };
    }
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = toEngineScoreLimit(config.target_score);
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit = config.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * solution.tasks.size(), 400, 3200);
    auto& moves = engineConfig.search.moves;
    moves.relocate_weight = config.relocate_weight;
    moves.list_swap_weight = config.list_swap_weight;
    moves.two_opt_weight = config.two_opt_weight;
    moves.or_opt_weight = config.or_opt_weight;
    moves.tail_swap_weight = config.tail_swap_weight;
    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruin_recreate_unimproved_step_trigger;
    lns.min_ruin_fraction = config.ruin_recreate_min_ruin_fraction;
    lns.max_ruin_fraction = config.ruin_recreate_max_ruin_fraction;
    lns.max_ruin_count = config.ruin_recreate_max_ruin_count;
    lns.operator_decay = config.ruin_recreate_operator_decay;
    lns.rewards = config.ruin_recreate_rewards;
    lns.polish_steps = config.ruin_recreate_polish_steps;
    return engineConfig;
}

// The domain Score of a plugin calculator, read from its three accumulators
// (level_scores(), before the BelowHard fold) rather than decoded from the
// folded engine hard: the decode would misfile a medium total of 10^9 or more
// as hard units and turns a saturated fold into nonsense. Only the soft level
// is still split into soft1 / soft2 by kMakespanUnit (exact at the default
// weights; see Score).
Score levelScoreOf(const optimaru::DecomposedObjective<TaskAssigningSolution>& calculator) {
    const auto levels = calculator.level_scores();
    const std::int64_t soft = optimaru::round_score_level(levels.soft, "soft");
    return {static_cast<std::int64_t>(levels.hard),
            optimaru::round_score_level(levels.medium, "medium"), soft / kMakespanUnit,
            soft % kMakespanUnit};
}

}  // namespace

void Solver::solve(TaskAssigningSolution& solution) {
    if (solution.tasks.empty() || solution.employees.empty()) {
        // Nothing to plan (or nobody to plan for): just score the input.
        ScoreCalculator::score(solution);
        return;
    }
    if (config_.ruin_recreate_rewards.size() != 4) {
        throw std::invalid_argument("ruin_recreate_rewards must contain exactly 4 values");
    }

    // Cheapest-insertion construction + late-acceptance local search over
    // the sequence moves (+ ALNS kicks), multi-start parallel when
    // parallel_solver_count > 1. Each start gets its own calculator instance
    // (the calculators are stateful).
    optimaru::Solver<TaskAssigningSolution> engine(
            buildEngineConfig(config_, solution, std::max(1, config_.parallel_solver_count)));
    engine.solve_multistart(
            solution, [] { return std::make_unique<TaskAssigningIncrementalScoreCalculator>(); });

    // The authoritative final score — a full recompute that also re-derives
    // the derived task fields (employee, startTime) on the best solution.
    ScoreCalculator::score(solution);
}

void Solver::solve(TaskAssigningSolution& solution,
                   optimaru::DecomposedObjective<TaskAssigningSolution>& calculator) {
    if (config_.ruin_recreate_rewards.size() != 4) {
        throw std::invalid_argument("ruin_recreate_rewards must contain exactly 4 values");
    }
    if (!solution.tasks.empty() && !solution.employees.empty()) {
        // Single-start: the caller-owned calculator cannot be cloned, so the
        // multi-start factory would have nothing to hand its extra workers.
        optimaru::Settings engineConfig = buildEngineConfig(config_, solution, 1);
        if (config_.on_new_best) {
            // The engine reports a new best right after the working solution
            // became it, so the calculator holds exactly that solution:
            // report its levels like the final solution.score below.
            auto callback = config_.on_new_best;
            engineConfig.on_new_best = [callback, &calculator](std::int64_t step,
                                                               const optimaru::Score&) {
                callback(step, levelScoreOf(calculator));
            };
        }
        auto engine = optimaru::Solver<TaskAssigningSolution>(std::move(engineConfig));
        engine.solve(solution, calculator);
    }

    // Recompute the derived task fields (employee, startTime) on the best
    // solution, then score it with the caller's constraint assembly (custom
    // weights included), leaving the calculator primed on it so
    // constraint_totals() explains exactly this score.
    ScoreCalculator::updateShadowVariables(solution);
    calculator.rebuild(solution);
    calculator.score();
    solution.score = levelScoreOf(calculator);
}

}  // namespace ta
