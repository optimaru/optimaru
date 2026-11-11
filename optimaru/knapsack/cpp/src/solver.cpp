#include "ks/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ks/construction.hpp"
#include "ks/incremental_calculator.hpp"
#include "ks/moves.hpp"
#include "ks/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver_config.hpp"

namespace ks {

namespace {

// The engine configuration shared by both solve() overloads and every parallel
// start: late-acceptance local search over the built-in assignment moves, plus
// the ALNS kicks. The engine's construction heuristic finds nothing left to
// place: the domain construction decided every item.
optimaru::Settings engineConfigOf(const Solver::Config& config, const KnapsackProblem& problem) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.items.size(), 400, 3200);
    // Restart late acceptance with a doubled history when the search stalls
    // (see AcceptanceSettings::history_growth): measured on every instance of
    // benchmarks/knapsack at 30 s on three seeds, one run of 21 worse (by 1 in 17,470).
    engineConfig.search.acceptance.history_growth = 2.0;
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;
    // CoreRepackMove costs a pass over the items and a table per draw, so a
    // step may draw it only this often; inside the gate the adaptive selector
    // chooses between it and the move mix.
    engineConfig.search.custom_move_probability = 0.001;

    // Nearby swap partners: items of similar weight. The weights are copied,
    // so the meter stays valid in the multi-start worker threads, whose
    // problems are copies.
    if (config.nearby_probability > 0.0 && config.nearby_size > 0) {
        auto weights = std::make_shared<std::vector<std::int64_t>>();
        for (const Item& item : problem.items) weights->push_back(item.weight);
        engineConfig.search.nearby.probability = config.nearby_probability;
        engineConfig.search.nearby.size = config.nearby_size;
        engineConfig.search.nearby.distance = [weights](int a, int b) {
            return static_cast<double>(std::llabs((*weights)[a] - (*weights)[b]));
        };
    }

    return engineConfig;
}

// Validates, constructs and consumes the seed.
void construct(KnapsackProblem& problem) {
    ScoreCalculator::prepare(problem);  // validates the facts and the selection
    constructSelection(problem);
    problem.warmStart = false;  // a seed is used once
}

// One single-start search of a constructed problem, with the domain's macro
// move (ks/moves.hpp) registered, driven by whichever calculator the caller
// brought. Returns the engine's best score.
Score runEngine(const Solver::Config& config, KnapsackProblem& problem,
                optimaru::Objective<KnapsackProblem>& calculator) {
    optimaru::Solver<KnapsackProblem> engine(engineConfigOf(config, problem));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    if (config.core_repack_moves && problem.itemCount() >= 2) {
        engine.add_move_operator(std::make_unique<CoreRepackMove>(problem));
    }
    return engine.solve(problem, calculator);
}

}  // namespace

void Solver::solve(KnapsackProblem& problem) {
    optimaru::check_config(config_);
    construct(problem);
    if (!problem.items.empty()) {
        // Independent parallel starts of this solver's own search: start i
        // searches its own copy of the constructed selection with seed + i.
        // The engine's multi-start cannot be used, because a registered
        // operator like CoreRepackMove is per-Solver and would be missing in
        // its worker threads. Each start leaves the engine's best score in
        // its copy, which is what the starts are compared by.
        optimaru::solve_parallel_starts(
                config_, problem, [](const Config& config, KnapsackProblem& candidate, int) {
                    KnapsackIncrementalScoreCalculator calculator;
                    candidate.score = runEngine(config, candidate, calculator);
                });
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::score(problem);
}

void Solver::solve(KnapsackProblem& problem,
                   optimaru::DecomposedObjective<KnapsackProblem>& calculator) {
    optimaru::check_config(config_);
    construct(problem);
    if (!problem.items.empty()) {
        // Single-start: the caller-owned calculator cannot be cloned.
        runEngine(config_, problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace ks
