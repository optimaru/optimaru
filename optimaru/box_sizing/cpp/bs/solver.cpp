#include "bs/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "bs/accept/reheating_late_acceptance.hpp"
#include "bs/construct/construction.hpp"
#include "bs/model/pricing.hpp"
#include "bs/score/incremental_calculator.hpp"
#include "bs/score/score_calculator.hpp"
#include "bs/search/moves/close_box_move.hpp"
#include "bs/search/moves/open_box_move.hpp"
#include "bs/search/moves/swap_box_move.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver.hpp"

namespace bs {

namespace {

// Validates, drops the input boxes of a plain solve and refuses what no
// assignment can make feasible. Leaves the problem ready for constructGreedy.
void prepareSolve(BoxSizingProblem& problem) {
    ScoreCalculator::prepare(problem);
    ScoreCalculator::requireSolvable(problem);
    if (!problem.warmStart) {
        for (Parcel& parcel : problem.parcels) parcel.box = -1;  // a plain solve ignores them
    }
    problem.warmStart = false;  // a seed is used once
}

// The prices a picked assembly gives the sizes: the weights of its
// voidVolumeCost, boxCost and boxTypeCost picks, each summed over its picks
// (a constraint left out weighs 0). Only the weights are read: a pick moved
// to the hard level, or shaped by an exponent or an offset, is priced as if
// it were soft and linear, which changes which steps are proposed and never
// how one is judged. An assembly that prices neither the void nor the cost
// still orders the sizes by volume: the smaller box first.
Pricing pricingOf(const optimaru::DecomposedObjective<BoxSizingProblem>& calculator) {
    Pricing pricing{0.0, 0.0, 0.0};
    for (const auto& constraint : calculator.unit_constraint_list()) {
        if (constraint->name == "voidVolumeCost") pricing.voidWeight += constraint->weight;
        if (constraint->name == "boxCost") pricing.costWeight += constraint->weight;
    }
    for (const auto& constraint : calculator.global_constraint_list()) {
        if (constraint->name == "boxTypeCost") pricing.typeWeight += constraint->weight;
    }
    if (pricing.voidWeight == 0.0 && pricing.costWeight == 0.0) pricing.voidWeight = 1.0;
    return pricing;
}

// The use case's own moves (in Python: config.custom_moves), which a step asks for
// after the built-in strategies (optimaru::Solver::choose_move). To add a move:
// write it in search/moves/ and register it here, behind a switch of the Config.
void registerMoves(optimaru::Solver<BoxSizingProblem>& engine, const Solver::Config& config,
                   const BoxSizingProblem& problem,
                   const std::shared_ptr<const BoxRanking>& ranking) {
    if (problem.boxCount() < 2) return;  // one candidate: nothing to decide
    if (config.close_box_moves) engine.add_move_operator(std::make_unique<CloseBoxMove>(ranking));
    if (config.open_box_moves) engine.add_move_operator(std::make_unique<OpenBoxMove>(ranking));
    if (config.swap_box_moves) engine.add_move_operator(std::make_unique<SwapBoxMove>(ranking));
}

// Shared engine wiring for both solve() overloads: late-acceptance local
// search and the domain's moves (and the ALNS kicks when the Config turns
// them on), driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly).
//
// There is no construction phase in the engine: constructGreedy has assigned
// every parcel before this is called, so the engine's own construction finds
// nothing left to place and the search starts at once.
void runEngine(const Solver::Config& config, BoxSizingProblem& problem,
               optimaru::Objective<BoxSizingProblem>& calculator,
               const std::shared_ptr<const BoxRanking>& ranking) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.boxes.size(), 100, 800);
    engineConfig.moves.change_weight = config.change_weight;
    engineConfig.moves.swap_weight = config.swap_weight;
    engineConfig.custom_moves.share = config.custom_move_probability;
    // The moves on the set of sizes and the move mix take turns evenly. The
    // engine's adaptive wheel rewards a move by its outcome, and here the
    // outcomes mislead it: a set move is turned down most of the time and is
    // still the only kind that reaches another set of sizes, while a mix move
    // that swaps two parcels of one size keeps the score and is "accepted".
    engineConfig.alns.selection_type = optimaru::SelectionScheme::Uniform;

    const std::size_t historyLength = engineConfig.acceptance.history_length;
    optimaru::Solver<BoxSizingProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    // Late acceptance that starts over from just below its best when it
    // stalls (bs/accept/reheating_late_acceptance.hpp): from the greedy
    // construction, plain late acceptance is a descent and nothing more.
    if (config.reheat) {
        engine.set_acceptor_factory([historyLength](const Score& initial) {
            return std::make_unique<ReheatingLateAcceptance>(historyLength, initial);
        });
    }
    registerMoves(engine, config, problem, ranking);
    engine.solve(problem, calculator);
}

// One start on a prepared problem, scored by the monolithic calculators.
void searchOneStart(const Solver::Config& config, BoxSizingProblem& problem) {
    const auto ranking = std::make_shared<const BoxRanking>(BoxRanking::of(problem, Pricing{}));
    constructGreedy(problem, *ranking);
    BoxSizingIncrementalScoreCalculator calculator;
    runEngine(config, problem, calculator, ranking);
    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::score(problem);
}

}  // namespace

void Solver::solve(BoxSizingProblem& problem) {
    optimaru::check_config(config_);
    prepareSolve(problem);
    if (problem.parcels.empty()) {
        ScoreCalculator::score(problem);
        return;
    }

    // Independent parallel starts of this solver's own construction and
    // search. The engine's multi-start cannot be used: a registered custom
    // operator like CloseBoxMove is per-Solver and stateful, so it would
    // silently vanish in the worker threads. Every start begins from the same
    // construction (it draws nothing) and searches with its own seed.
    optimaru::solve_parallel_starts(config_, problem,
                                    [](const Config& config, BoxSizingProblem& candidate, int) {
                                        searchOneStart(config, candidate);
                                    });
}

void Solver::solve(BoxSizingProblem& problem,
                   optimaru::DecomposedObjective<BoxSizingProblem>& calculator) {
    optimaru::check_config(config_);
    prepareSolve(problem);
    if (!problem.parcels.empty()) {
        const auto ranking =
                std::make_shared<const BoxRanking>(BoxRanking::of(problem, pricingOf(calculator)));
        constructGreedy(problem, *ranking);
        runEngine(config_, problem, calculator, ranking);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly that score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace bs
