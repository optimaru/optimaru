#include "qap/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/accept/acceptors.hpp"
#include "qap/incremental_calculator.hpp"
#include "qap/score_calculator.hpp"

namespace qap {

namespace {

// The engine configuration shared by both solve() overloads: swap moves only
// (change_weight 0 with a positive swap weight; the engine falls back to a
// change move only when BOTH are 0, so the swap weight stays 1), every
// value-assigning strategy off (their engine defaults are off already; set
// here so a future default cannot switch them on behind the permutation's
// back). The engine's construction heuristic finds nothing to place: the
// start is a complete permutation.
optimaru::Settings engineConfigOf(const Solver::Config& config,
                                  const QuadraticAssignmentProblem& problem, int starts) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = starts;

    auto& acceptor = engineConfig.search.acceptance;
    acceptor.rule = config.rule;
    acceptor.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.facilities.size(), 400, 3200);
    acceptor.tabu_tenure = config.tabu_tenure;
    acceptor.start_temperature = config.start_temperature;

    auto& local = engineConfig.search;
    local.moves.change_weight = 0.0;
    local.moves.swap_weight = 1.0;
    local.ruin_recreate.enabled = false;
    local.ruin_recreate.as_move_weight = 0.0;
    local.descent_weight = 0.0;
    local.route_elimination_weight = 0.0;
    local.ejection_chain_weight = 0.0;
    local.iterated_local_search_weight = 0.0;
    local.value_hint = nullptr;
    local.entity_hint = nullptr;
    return engineConfig;
}

void requireConfig(const Solver::Config& config) {
    optimaru::check_config(config);
    if (config.parallel_solver_count <= 0)
        throw std::invalid_argument("parallel_solver_count must be positive");
    if (config.tabu_tenure < 0) throw std::invalid_argument("entityTabuSize must be >= 0");
    if (!(config.simulated_annealing_temperature_fraction > 0.0))
        throw std::invalid_argument("simulated_annealing_temperature_fraction must be > 0");
}

// SIMULATED_ANNEALING with the auto starting temperature (0, 0): the
// acceptor factory computes it from the start's score. The engine shares the
// factory with every multi-start start. (An explicit temperature goes into the
// engine config, see engineConfigOf.)
bool autoTemperature(const Solver::Config& config) {
    return config.rule == optimaru::AcceptanceRule::SimulatedAnnealing &&
           config.start_temperature.hard == 0 && config.start_temperature.soft == 0.0;
}

void installAutoTemperature(const Solver::Config& config,
                            optimaru::Solver<QuadraticAssignmentProblem>& engine) {
    if (!autoTemperature(config)) return;
    const double fraction = config.simulated_annealing_temperature_fraction;
    engine.set_acceptor_factory(
            [fraction](const optimaru::Score& start) -> std::unique_ptr<optimaru::Acceptor> {
                const double soft = std::max(1.0, fraction * std::fabs(start.soft));
                return std::make_unique<optimaru::SimulatedAnnealing>(optimaru::Score{0, soft});
            });
}

// Validates and lays out the start: the seed (a full bijection) once, else
// the identity. Returns false when there is nothing to search (n <= 1).
bool start(QuadraticAssignmentProblem& problem) {
    ScoreCalculator::prepare(problem);  // validates the facts
    if (problem.warmStart) {
        if (!problem.isBijection()) {
            throw std::invalid_argument(
                    "warm start needs a full bijection: every facility at its own location");
        }
    } else {
        for (int f = 0; f < problem.facilityCount(); ++f) problem.facilities[f].location = f;
    }
    problem.warmStart = false;  // a seed is used once
    return problem.facilityCount() > 1;
}

void requireBijection(const QuadraticAssignmentProblem& problem) {
    if (!problem.isBijection())
        throw std::logic_error("quadratic-assignment solve left a non-bijective assignment");
}

}  // namespace

void Solver::solve(QuadraticAssignmentProblem& problem) {
    requireConfig(config_);
    if (start(problem)) {
        // Independent parallel starts through the engine's own multi-start:
        // no custom operator or variable listener is registered, so the
        // worker threads run exactly the same search (seed + i each).
        const int starts = std::max(1, config_.parallel_solver_count);
        optimaru::Solver<QuadraticAssignmentProblem> engine(
                engineConfigOf(config_, problem, starts));
        installAutoTemperature(config_, engine);
        engine.solve_multistart(problem, [] {
            return std::make_unique<QuadraticAssignmentIncrementalScoreCalculator>();
        });
    }
    requireBijection(problem);
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::score(problem);
}

void Solver::solve(QuadraticAssignmentProblem& problem,
                   optimaru::DecomposedObjective<QuadraticAssignmentProblem>& calculator) {
    requireConfig(config_);
    if (start(problem)) {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<QuadraticAssignmentProblem> engine(engineConfigOf(config_, problem, 1));
        installAutoTemperature(config_, engine);
        engine.solve(problem, calculator);
    }
    requireBijection(problem);
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace qap
