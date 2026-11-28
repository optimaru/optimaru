#include "cl/solver.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

#include "cl/constraints.hpp"
#include "cl/construct/construction.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/search/moves/empty_container.hpp"
#include "cl/search/moves/repack_pair.hpp"
#include "cl/search/moves/switch_container.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver.hpp"

namespace cl {

namespace {

// The use case's own moves (in Python: config.custom_moves), which a step asks for
// after the built-in strategies (optimaru::Solver::choose_move). To add a move:
// write it in search/moves/ and register it here, behind a switch of the Config.
void registerMoves(optimaru::Solver<ContainerLoadingProblem>& engine, const Solver::Config& config,
                   const ContainerLoadingProblem& problem) {
    if (problem.containers.size() < 2) return;  // nowhere to move a load to
    if (config.empty_container_moves) {
        engine.add_move_operator(
                std::make_unique<EmptyContainerMove>(problem, config.empty_container_candidates));
    }
    if (config.repack_moves) engine.add_move_operator(std::make_unique<RepackPairMove>(problem));
    if (config.switch_container_moves && SwitchContainerMove::applies(problem))
        engine.add_move_operator(std::make_unique<SwitchContainerMove>(problem));
}

// Shared engine wiring for both solve() overloads: late-acceptance local
// search over the sequence move mix (+ ALNS kicks) and the domain's moves,
// driven by whichever assembly the caller brought.
void runEngine(const Solver::Config& config, ContainerLoadingProblem& problem,
               optimaru::Objective<ContainerLoadingProblem>& calculator) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.run.parallel_starts = 1;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(4 * problem.items.size(), 200, 1600);
    engineConfig.moves.relocate_weight = config.relocate_weight;
    engineConfig.moves.list_swap_weight = config.list_swap_weight;
    engineConfig.moves.two_opt_weight = config.two_opt_weight;
    engineConfig.moves.or_opt_weight = config.or_opt_weight;

    optimaru::Solver<ContainerLoadingProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    registerMoves(engine, config, problem);
    engine.solve(problem, calculator);
}

// One start on a prepared problem, scored by the default assembly.
void searchOneStart(const Solver::Config& config, ContainerLoadingProblem& problem) {
    if (config.first_fit_decreasing) constructLoading(problem);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    runEngine(config, problem, calculator);
    // The authoritative final score: a full recompute on the best solution,
    // which also writes where every item stands.
    ScoreCalculator::score(problem);
}

}  // namespace

void Solver::solve(ContainerLoadingProblem& problem) {
    optimaru::check_config(config_);
    ScoreCalculator::prepare(problem);
    if (problem.items.empty() || problem.containers.empty()) {
        ScoreCalculator::score(problem);
        return;
    }
    // Independent parallel starts of this solver's own single-start solve.
    // The engine's multi-start cannot be used: a registered operator like
    // EmptyContainerMove is per-Solver and keeps decodes of its own, so it
    // would be missing from the worker threads.
    optimaru::solve_parallel_starts(
            config_, problem, [](const Config& config, ContainerLoadingProblem& candidate, int) {
                searchOneStart(config, candidate);
            });
}

void Solver::solve(ContainerLoadingProblem& problem,
                   optimaru::DecomposedObjective<ContainerLoadingProblem>& calculator) {
    optimaru::check_config(config_);
    ScoreCalculator::prepare(problem);
    if (!problem.items.empty() && !problem.containers.empty()) {
        if (config_.first_fit_decreasing) constructLoading(problem);
        runEngine(config_, problem, calculator);
    }
    // Where every item stands on the best solution, then its score by the
    // caller's constraint assembly (custom weights included), leaving the
    // calculator primed on it so constraint_totals() explains exactly this
    // score.
    ScoreCalculator::updateDerived(problem);
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace cl
