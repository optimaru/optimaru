#include "bp/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bp/construction.hpp"
#include "bp/incremental_calculator.hpp"
#include "bp/moves.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"
#include "optimaru/search/operators.hpp"

namespace bp {

namespace {

void requireRewards(const Solver::Config& config) {
    if (config.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
}

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
// search (+ ALNS kicks), plus the domain's macro moves (bp/moves.hpp) and
// value recommender, driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly). The engine's construction heuristic
// finds nothing left to place: constructBins assigned every item.
void runEngine(const Solver::Config& config, BinPackingProblem& problem,
               optimaru::Objective<BinPackingProblem>& calculator) {
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.on_new_best = config.on_new_best;
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = config.target_score;
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit = config.stall_step_limit;
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
    if (config.valueRecommenderProbability > 0.0 && problem.itemCount() >= 2) {
        const BinPackingProblem& live = problem;
        const double p = config.valueRecommenderProbability;
        engineConfig.search.value_hint = [&live, p](std::mt19937_64& rng, int item) -> int {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p) return -1;
            const auto n = static_cast<std::uint64_t>(live.items.size());
            int other = static_cast<int>(rng() % (n - 1));
            if (other >= item) ++other;    // never the item itself
            return live.items[other].bin;  // may be -1 inside a ruin: uniform
        };
    }

    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polish_steps = config.ruinRecreatePolishSteps;
    lns.as_move_weight = config.ruinRecreateAsMoveWeight;
    lns.as_move_max_ruin = config.ruinRecreateAsMoveMaxRuin;

    optimaru::Solver<BinPackingProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    if (config.emptyBinMoves && problem.itemCount() >= 2)
        engine.add_move_operator(std::make_unique<EmptyBinMove>(config.emptyBinCandidates));
    if (config.repackMoves && problem.itemCount() >= 2) {
        engine.add_move_operator(
                std::make_unique<RepackMove>(config.emptyBinCandidates, config.repackPartnerBins));
    }
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
    requireRewards(config_);
    prepareSolve(problem);
    problem.warmStart = false;  // a seed is used once
    if (problem.items.empty()) {
        ScoreCalculator::score(problem);
        return;
    }

    // Independent parallel starts: worker i constructs and searches its own
    // copy with seed + i. The engine's own multi-start cannot be used,
    // because a registered custom operator like EmptyBinMove is per-Solver
    // and stateful, so it would silently vanish in the worker threads. The
    // best score wins (the lowest worker index on ties).
    const int workerCount = std::max(1, config_.parallelSolverCount);
    const std::vector<int> descending = descendingOrder(problem);
    const bool seeded = std::any_of(problem.items.begin(), problem.items.end(),
                                    [](const Item& item) { return item.bin >= 0; });
    if (workerCount == 1) {
        searchOneStart(config_, problem, descending);
        return;
    }

    std::vector<BinPackingProblem> candidates(workerCount, problem);
    std::vector<std::exception_ptr> failures(workerCount);
    // Raised by the first worker that fails: every worker's engine checks
    // it once a step, so the others stop instead of using up their budget.
    auto stop = std::make_shared<std::atomic<bool>>(false);
    // A worker that cannot be started (std::system_error) stops and joins
    // the ones already running before the error is rethrown here.
    optimaru::run_worker_threads(
            workerCount, *stop, [this, seeded, &descending, &candidates, &failures, &stop](int i) {
                try {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stop_flag = stop;
                    workerConfig.seed += static_cast<std::uint64_t>(i);
                    if (i != 0) workerConfig.on_new_best = nullptr;
                    std::mt19937_64 rng(workerConfig.seed);
                    const std::vector<int> order =
                            (i == 0 || seeded) ? descending : tieShuffledOrder(candidates[i], rng);
                    searchOneStart(workerConfig, candidates[i], order);
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
    problem = std::move(candidates[best]);
}

void Solver::solve(BinPackingProblem& problem,
                   optimaru::DecomposedObjective<BinPackingProblem>& calculator) {
    requireRewards(config_);
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
