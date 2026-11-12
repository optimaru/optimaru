#include "ba/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "ba/incremental_calculator.hpp"
#include "ba/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace ba {

namespace {

// Translates a domain Config into the engine's Settings: termination,
// late acceptance, the move mix, the ALNS knobs and the nearby distance
// meter. The meter closes over `plan`, so the caller must keep that solution
// alive for the whole solve.
optimaru::Settings configureEngine(const Solver::Config& config, const BedPlan& plan) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    if (config.on_new_best) {
        auto on_new_best = config.on_new_best;
        engineConfig.on_new_best = [on_new_best](std::int64_t step, const optimaru::Score& score) {
            on_new_best(step, fromEngineScore(score));
        };
    }
    engineConfig.limits.target_score = toEngineScoreLimit(config.target_score);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * plan.stays.size(), 8000, 12000);
    // Restart late acceptance with a doubled history when the search stalls
    // (see AcceptanceSettings::history_growth). Measured on the six instances
    // benchmarks/bed-allocation had before its ladder grew to 20 (small, demo,
    // medium, medium-2, large, xlarge), at 30 s on three seeds: the mean soft score
    // is 2.8%, 1.7% and 3.7% better up to 183 stays (the 20-stay instance ends
    // on one value on every seed), and within 0.3% either way from 180 stays
    // up, where few stages fit.
    // A solve that starts from a solution the caller gave has no stages: the
    // start's score is then no lower than where the search stands, and a stage
    // would take the place of the kicks that find something there.
    if (optimaru::nothing_placed(plan)) engineConfig.search.acceptance.history_growth = 2.0;
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    // Exhaustive assignment descent at the budget-share rule:
    // weight x value range = 0.2 (the small-value-range regime).
    engineConfig.search.descent_weight = 0.2 / static_cast<double>(plan.beds.size() + 1);

    // Nearby selection over the stays' date windows: two stays are close when
    // they occupy nearly the same nights. Such a pair can trade beds without
    // disturbing anyone else's occupancy, which is what makes a swap move
    // useful here, and the LNS related ruin then removes one temporal cluster
    // (the contended nights) instead of a scattered set.
    if (config.nearby_probability > 0.0) {
        const BedPlan* planPtr = &plan;
        engineConfig.search.nearby.distance = [planPtr](int a, int b) {
            const Stay& x = planPtr->stays[a];
            const Stay& y = planPtr->stays[b];
            return static_cast<double>(std::abs(x.arrivalDay - y.arrivalDay) +
                                       std::abs(x.departureDay - y.departureDay));
        };
        engineConfig.search.nearby.size = std::max(1, config.nearby_size);
        engineConfig.search.nearby.probability = config.nearby_probability;
    }
    return engineConfig;
}

// The domain Score of a plugin calculator, read from its three level
// accumulators (level_scores(), before the medium fold) rather than decoded
// from the folded engine soft, which would file a soft total of 10^9 or more
// (or the fraction of a non-integral medium total) under the wrong level.
Score levelScoreOf(const optimaru::DecomposedObjective<BedPlan>& calculator) {
    const auto levels = calculator.level_scores();
    return Score{static_cast<std::int64_t>(levels.hard),
                 optimaru::round_score_level(levels.medium, "medium"),
                 optimaru::round_score_level(levels.soft, "soft")};
}

// One single-threaded solve on the monolithic fast path. The calculator
// instance is this solve's own; the engine resets it on the working solution
// at solve start. Its fixed default weights keep every level integral and,
// below ~4.3 million stay-nights, the soft total under 10^9 (the Score adapter
// in domain.hpp), so the engine score on_new_best decodes has exactly the levels
// of the final full recompute.
void solveSingle(const Solver::Config& config, BedPlan& plan) {
    optimaru::Settings engineConfig = configureEngine(config, plan);

    BedAllocationIncrementalScoreCalculator calculator;
    optimaru::Solver<BedPlan> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    engine.solve(plan, calculator);
}

// One single-threaded solve driven by a caller-assembled plugin calculator.
// on_new_best reports the calculator's own levels, the same way as the final
// plan.score of Solver::solve(plan, calculator): the engine announces a new
// best right after the working solution became it, so the accumulators hold
// exactly that solution at that moment.
void solveSingle(const Solver::Config& config, BedPlan& plan,
                 optimaru::DecomposedObjective<BedPlan>& calculator) {
    optimaru::Settings engineConfig = configureEngine(config, plan);
    if (config.on_new_best) {
        auto on_new_best = config.on_new_best;
        engineConfig.on_new_best = [on_new_best, &calculator](std::int64_t step,
                                                              const optimaru::Score&) {
            on_new_best(step, levelScoreOf(calculator));
        };
    }

    optimaru::Solver<BedPlan> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    engine.solve(plan, calculator);
}

}  // namespace

void Solver::solve(BedPlan& plan) {
    ScoreCalculator::prepare(plan);  // intern equipment ids / priorities
    if (plan.stays.empty()) {
        ScoreCalculator::score(plan);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts of this solver's own search rather than the
    // engine's multi-start: every start solves its own copy with its own
    // calculator and its own engine config, so the nearby distance meter reads
    // the very plan its start solves instead of a solution shared across
    // threads.
    const auto solveOne = [](const Config& config, BedPlan& candidate, int) {
        // Construction heuristic + late-acceptance local search (+ ALNS kicks).
        solveSingle(config, candidate);
        // The authoritative final score: a full recompute on the best solution.
        ScoreCalculator::score(candidate);
    };
    optimaru::solve_parallel_starts(config_, plan, solveOne);
}

void Solver::solve(BedPlan& plan, optimaru::DecomposedObjective<BedPlan>& calculator) {
    ScoreCalculator::prepare(plan);  // intern equipment ids / priorities
    optimaru::check_config(config_);
    if (!plan.stays.empty()) solveSingle(config_, plan, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights and levels included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score. The three levels come
    // straight from the calculator's accumulators (level_scores(), before the
    // medium fold) rather than from decoding the folded engine score, so a
    // demoted or re-weighted term lands on the level the assembly gave it.
    // on_new_best reported the scores of this solve the same way.
    calculator.rebuild(plan);
    plan.score = levelScoreOf(calculator);
}

}  // namespace ba
