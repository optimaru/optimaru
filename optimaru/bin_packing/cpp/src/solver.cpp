#include "bp/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bp/construction.hpp"
#include "bp/fill_late_acceptance.hpp"
#include "bp/incremental_calculator.hpp"
#include "bp/moves.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/operators.hpp"

namespace bp {

namespace {

// The problem checks, in a fixed order and wording.
void validate(const BinPackingProblem& problem) {
    if (problem.binCapacity <= 0) throw std::invalid_argument("bin_capacity must be positive");
    std::unordered_set<std::string> ids;
    for (const Item& item : problem.items) {
        if (!ids.insert(item.id).second) throw std::invalid_argument("item ids must be unique");
    }
    for (const Item& item : problem.items) {
        if (item.weight <= 0) throw std::invalid_argument("item weights must be positive");
    }
}

// Validates, drops the input bins of a plain solve and builds the derived
// lookups. Leaves the problem ready for constructBins.
void prepareSolve(BinPackingProblem& problem) {
    validate(problem);
    if (!problem.warmStart) {
        for (Item& item : problem.items) item.bin = -1;  // a plain solve ignores the input bins
    }
    ScoreCalculator::prepare(problem);
}

// Shared engine wiring for both solve() overloads: late-acceptance local
// search (+ ALNS kicks), plus the domain's macro moves (bp/moves.hpp), its
// rule for steps that keep the score (bp/fill_late_acceptance.hpp) and
// value recommender, driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly). The engine's construction heuristic
// finds nothing left to place: constructBins assigned every item.
void runEngine(const Solver::Config& config, BinPackingProblem& problem,
               optimaru::Objective<BinPackingProblem>& calculator) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.items.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.custom_move_probability = config.custom_move_probability;
    engineConfig.search.descent_weight = config.descent_weight;

    // Guided change-move targets: the bin of a uniformly drawn other item (a
    // bin in use; -1 falls back to the engine's uniform draw over all n
    // bins). The engine solves in place, so the problem captured here IS the
    // working solution and the recommender reads the live bins.
    if (config.value_recommender_probability > 0.0 && problem.itemCount() >= 2) {
        const BinPackingProblem& live = problem;
        const double p = config.value_recommender_probability;
        engineConfig.search.value_hint = [&live, p](std::mt19937_64& rng, int item) -> int {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p) return -1;
            const auto n = static_cast<std::uint64_t>(live.items.size());
            int other = static_cast<int>(rng() % (n - 1));
            if (other >= item) ++other;    // never the item itself
            return live.items[other].bin;  // may be -1 inside a ruin: uniform
        };
    }

    const std::size_t historyLength = engineConfig.search.acceptance.history_length;
    optimaru::Solver<BinPackingProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    // Late acceptance that compares equal scores by how full the bins are.
    // The engine solves in place, so the acceptor reads the working solution.
    // A caller's acceptor takes the place of the configured rule and of
    // history_growth: bin packing lets the caller choose neither, and one
    // that ever does has to leave this factory out for any rule but plain
    // late acceptance. The factory reads this start's own problem, which is
    // right as long as every start has its own engine (solve_parallel_starts
    // above): the engine's multi-start would hand one factory to the copies
    // it solves, and every acceptor would read the caller's packing.
    if (config.fill_tie_break) {
        const BinPackingProblem& live = problem;
        engine.set_acceptor_factory([&live, historyLength](const Score& initial) {
            return std::make_unique<FillLateAcceptance>(historyLength, initial, live);
        });
    }
    if (config.empty_bin_moves && problem.itemCount() >= 2)
        engine.add_move_operator(std::make_unique<EmptyBinMove>(config.empty_bin_candidates));
    if (config.repack_moves && problem.itemCount() >= 2) {
        engine.add_move_operator(std::make_unique<RepackMove>(config.empty_bin_candidates,
                                                              config.repack_partner_bins));
    }
    if (config.trade_moves && problem.itemCount() >= 2)
        engine.add_move_operator(std::make_unique<TradeMove>(config.empty_bin_candidates));
    engine.solve(problem, calculator);
}

// After the search: relabel the bins 0..k-1 when anything moved (a result
// equal to its start keeps its labels, so a zero-budget seed comes back as
// it was).
void compactIfMoved(BinPackingProblem& problem, const std::vector<int>& start) {
    if (problem.assignment() != start) compactBins(problem);
}

// One start on a prepared problem, scored by the monolithic calculators.
void searchOneStart(const Solver::Config& config, BinPackingProblem& problem,
                    const std::vector<int>& order) {
    constructBins(problem, order);
    const std::vector<int> start = problem.assignment();
    BinPackingIncrementalScoreCalculator calculator;
    runEngine(config, problem, calculator);
    compactIfMoved(problem, start);
    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::score(problem);
}

}  // namespace

void Solver::solve(BinPackingProblem& problem) {
    optimaru::check_config(config_);
    prepareSolve(problem);
    problem.warmStart = false;  // a seed is used once
    if (problem.items.empty()) {
        ScoreCalculator::score(problem);
        return;
    }

    // Independent parallel starts of this solver's own construction and
    // search. The engine's multi-start cannot be used: a registered custom
    // operator like EmptyBinMove is per-Solver and stateful, so it would
    // silently vanish in the worker threads. Start 0, and every start of a
    // seeded run, constructs in the descending order; start i > 0 shuffles
    // the items of equal weight with an rng of its own seed.
    const std::vector<int> descending = descendingOrder(problem);
    const bool seeded = std::any_of(problem.items.begin(), problem.items.end(),
                                    [](const Item& item) { return item.bin >= 0; });
    optimaru::solve_parallel_starts(
            config_, problem,
            [seeded, &descending](const Config& config, BinPackingProblem& candidate, int i) {
                std::mt19937_64 rng(config.seed);
                const std::vector<int> order =
                        (i == 0 || seeded) ? descending : tieShuffledOrder(candidate, rng);
                searchOneStart(config, candidate, order);
            });
}

void Solver::solve(BinPackingProblem& problem,
                   optimaru::DecomposedObjective<BinPackingProblem>& calculator) {
    optimaru::check_config(config_);
    prepareSolve(problem);
    problem.warmStart = false;  // a seed is used once
    if (!problem.items.empty()) {
        constructBins(problem, descendingOrder(problem));
        const std::vector<int> start = problem.assignment();
        runEngine(config_, problem, calculator);
        compactIfMoved(problem, start);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace bp
