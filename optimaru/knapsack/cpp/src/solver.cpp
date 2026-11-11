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
#include "ks/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace ks {

namespace {

// The engine configuration shared by both solve() overloads: late-acceptance
// local search over the built-in assignment moves, plus the ALNS kicks. The
// engine's construction heuristic finds nothing left to place: the domain
// construction decided every item.
optimaru::Settings engineConfigOf(const Solver::Config& config, const KnapsackProblem& problem,
                                  int starts) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = starts;
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.items.size(), 400, 3200);
    // Restart late acceptance with a doubled history when the search stalls
    // (see AcceptanceSettings::history_growth): measured on every instance of
    // benchmarks/knapsack, none worse at 30 s.
    engineConfig.search.acceptance.history_growth = 2.0;
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;

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

}  // namespace

void Solver::solve(KnapsackProblem& problem) {
    optimaru::check_config(config_);
    construct(problem);
    if (!problem.items.empty()) {
        // Independent parallel starts through the engine's own multi-start:
        // no custom operator is registered, so the worker threads run exactly
        // the same search (seed + i each).
        const int starts = std::max(1, config_.parallel_solver_count);
        optimaru::Solver<KnapsackProblem> engine(engineConfigOf(config_, problem, starts));
        engine.solve_multistart(
                problem, [] { return std::make_unique<KnapsackIncrementalScoreCalculator>(); });
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
        optimaru::Solver<KnapsackProblem> engine(engineConfigOf(config_, problem, 1));
        engine.solve(problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace ks
