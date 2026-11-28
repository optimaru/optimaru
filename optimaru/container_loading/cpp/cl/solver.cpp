#include "cl/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "cl/constraints.hpp"
#include "cl/construct/construction.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/search/moves/empty_container.hpp"
#include "cl/search/moves/repack_pair.hpp"
#include "cl/search/moves/stop_order.hpp"
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
    if (config.stop_order_moves && StopOrderMove::applies(problem))
        engine.add_move_operator(std::make_unique<StopOrderMove>());
    if (problem.containers.size() < 2) return;  // nowhere to move a load to
    if (config.empty_container_moves) {
        engine.add_move_operator(
                std::make_unique<EmptyContainerMove>(problem, config.empty_container_candidates));
    }
    if (config.repack_moves) engine.add_move_operator(std::make_unique<RepackPairMove>(problem));
    if (config.switch_container_moves && SwitchContainerMove::applies(problem))
        engine.add_move_operator(std::make_unique<SwitchContainerMove>(problem));
}

// What the decoder weighs when an item can only break an order or a limit:
// the weights the constraints it is scored with put on leaving it out, on an
// unloading pair and on a unit of weight over a limit, per level. A
// constraint picked twice counts twice; one left out counts nothing. Only the
// weight and the level are read: each pick counts as linear in its raw cost
// and as a penalty, whatever its exponent, offset or clip.
void setBreakWeights(ContainerLoadingProblem& problem,
                     const optimaru::DecomposedObjective<ContainerLoadingProblem>& calculator) {
    BreakWeights weights;
    for (int l = 0; l < 3; ++l)
        weights.unplaced[l] = weights.blocking[l] = weights.overload[l] = weights.stacking[l] =
                weights.optional[l] = weights.segregation[l] = 0.0;
    for (const auto& constraint : calculator.unit_constraint_list()) {
        double* target = nullptr;
        if (constraint->name == "unplacedItemCost") target = weights.unplaced;
        if (constraint->name == "unloadingOrderCost") target = weights.blocking;
        if (constraint->name == "stackingLoadCost") target = weights.overload;
        if (constraint->name == "stackingRuleCost") target = weights.stacking;
        if (constraint->name == "optionalItemCost") target = weights.optional;
        if (constraint->name == "segregationCost") target = weights.segregation;
        if (target != nullptr) target[static_cast<int>(constraint->level)] += constraint->weight;
    }
    problem.breakWeights = weights;
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
    // Only for loads that are mostly ordered (vehicle loads): where most items
    // have no order, the distance would be volume alone, and a kick would try
    // only the places next to the items closest in volume.
    std::size_t ordered = 0;
    for (const Item& item : problem.items) ordered += item.unloadOrder ? 1 : 0;
    if (2 * ordered >= problem.items.size() && config.nearby_probability > 0.0) {
        // Close = the same stop, then the stops next to it; among equals,
        // the closest in volume. An item without an order is far from every
        // item with one (it has no block to keep).
        // The engine lives in this function, so the problem outlives it.
        const ContainerLoadingProblem* facts = &problem;
        engineConfig.selection.nearby.distance = [facts](int a, int b) {
            const Item& x = facts->items[static_cast<std::size_t>(a)];
            const Item& y = facts->items[static_cast<std::size_t>(b)];
            const double volumes =
                    std::abs(static_cast<double>(x.volume()) - static_cast<double>(y.volume())) /
                    (static_cast<double>(std::max(x.volume(), y.volume())) + 1.0);
            // Farther than any two stops can be (two ints differ by less).
            constexpr double kOtherKind = 1e12;
            if (x.unloadOrder.has_value() != y.unloadOrder.has_value()) return kOtherKind + volumes;
            if (!x.unloadOrder) return volumes;
            return static_cast<double>(
                           std::abs(static_cast<std::int64_t>(*x.unloadOrder) - *y.unloadOrder)) +
                   volumes;
        };
        engineConfig.selection.nearby.size = std::max(1, config.nearby_size);
        engineConfig.selection.nearby.probability = config.nearby_probability;
    }

    optimaru::Solver<ContainerLoadingProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    registerMoves(engine, config, problem);
    engine.solve(problem, calculator);
}

// One start on a prepared problem, scored by the default assembly.
void searchOneStart(const Solver::Config& config, ContainerLoadingProblem& problem) {
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    setBreakWeights(problem, calculator);
    if (config.first_fit_decreasing) constructLoading(problem);
    runEngine(config, problem, calculator);
    // The authoritative final score: a full recompute on the best solution,
    // which also writes where every item stands.
    ScoreCalculator::score(problem);
}

}  // namespace

void Solver::solve(ContainerLoadingProblem& problem) {
    optimaru::check_config(config_);
    ScoreCalculator::prepare(problem);
    // The default constraints' weights, also for the final score below
    // (every start sets the same on its copy).
    problem.breakWeights = BreakWeights();
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
    setBreakWeights(problem, calculator);
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
