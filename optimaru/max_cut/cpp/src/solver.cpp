#include "mc/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mc/incremental_calculator.hpp"
#include "mc/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

namespace mc {

namespace {

void requireRewards(const Solver::Config& config) {
    if (config.ruin_recreate_rewards.size() != 4)
        throw std::invalid_argument("ruin_recreate_rewards must contain exactly 4 values");
}

// The engine configuration of one search, shared by both solve() overloads
// and every parallel start: late-acceptance local search over the built-in
// assignment moves, plus the ALNS kicks. The engine's construction heuristic finds
// nothing left to place: the start partition assigned every vertex.
optimaru::Settings engineConfigOf(const Solver::Config& config, const MaxCutProblem& problem) {
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.on_new_best = config.on_new_best;
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = config.target_score;
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit = config.stall_step_limit;
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.vertexIds.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;

    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruin_recreate_unimproved_step_trigger;
    lns.min_ruin_fraction = config.ruin_recreate_min_ruin_fraction;
    lns.max_ruin_fraction = config.ruin_recreate_max_ruin_fraction;
    lns.max_ruin_count = config.ruin_recreate_max_ruin_count;
    lns.operator_decay = config.ruin_recreate_operator_decay;
    lns.rewards = config.ruin_recreate_rewards;
    lns.polish_steps = config.ruin_recreate_polish_steps;
    lns.as_move_weight = config.ruin_recreate_as_move_weight;
    lns.as_move_max_ruin = config.ruin_recreate_as_move_max_ruin;

    if (problem.exactSize()) {
        // Swaps only: every other move (a flip, a ruin's unassign-and-recreate,
        // a descent's change) could change a side size.
        engineConfig.search.moves.change_weight = 0.0;
        engineConfig.search.moves.swap_weight = 1.0;
        engineConfig.search.descent_weight = 0.0;
        lns.enabled = false;
        lns.as_move_weight = 0.0;
    }
    return engineConfig;
}

// Validates, builds the start partition and consumes the seed.
void construct(MaxCutProblem& problem) {
    ScoreCalculator::prepare(problem);  // validates the facts and the partition
    if (!problem.warmStart) std::fill(problem.side.begin(), problem.side.end(), kUnassigned);
    for (int v = 0; v < problem.vertexCount(); ++v) {
        if (problem.side[v] == kUnassigned)
            problem.side[v] = problem.is_pinned(v) ? problem.fixedSide[v] : v % kSideCount;
    }
    problem.warmStart = false;  // a seed is used once
}

// Whether the search has anything to do: an empty graph has no move, and in
// exact-size mode a swap needs two free vertices (the engine's fallback
// for fewer entities is a change move, which would break the size).
bool searchable(const MaxCutProblem& problem) {
    if (problem.vertexCount() == 0) return false;
    return !problem.exactSize() || problem.movableCount() >= 2;
}

// Independent parallel starts: worker i searches its own copy of the start
// partition with seed + i and the full budget (only worker 0 reports
// on_new_best); the best score wins, the lowest index on a tie. This is the
// search of the engine's solve_multistart, run on worker threads that catch
// their own exceptions: a throwing on_new_best, a ScoreChecks::Full
// ScoreMismatchError or a bad_alloc in one worker stops the others early
// and is rethrown here once every worker has joined, instead of terminating
// the process.
void searchInParallel(const Solver::Config& config, MaxCutProblem& problem, int starts) {
    std::vector<MaxCutProblem> copies(starts, problem);
    std::vector<optimaru::Score> scores(starts);
    std::vector<std::exception_ptr> failures(starts);
    // Every engine exists before any worker starts, so a failing worker can
    // stop all of them (request_stop() is thread-safe and sticky).
    std::vector<std::unique_ptr<optimaru::Solver<MaxCutProblem>>> engines;
    engines.reserve(starts);
    for (int i = 0; i < starts; ++i) {
        optimaru::Settings engineConfig = engineConfigOf(config, problem);
        engineConfig.seed += static_cast<std::uint64_t>(i);
        if (i != 0) engineConfig.on_new_best = nullptr;
        engines.push_back(
                std::make_unique<optimaru::Solver<MaxCutProblem>>(std::move(engineConfig)));
    }
    // A worker that cannot be started (std::system_error) stops and joins the
    // ones already running before the error is rethrown here.
    const auto stopAll = [&engines] {
        for (const auto& engine : engines) engine->request_stop();
    };
    optimaru::run_worker_threads(
            starts,
            [&copies, &scores, &failures, &engines, &stopAll](int i) {
                try {
                    MaxCutIncrementalScoreCalculator calculator;
                    scores[i] = engines[i]->solve(copies[i], calculator);
                } catch (...) {
                    failures[i] = std::current_exception();
                    stopAll();
                }
            },
            stopAll);
    for (const std::exception_ptr& failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }
    int best = 0;
    for (int i = 1; i < starts; ++i) {
        if (scores[i] > scores[best]) best = i;
    }
    problem = std::move(copies[best]);
}

}  // namespace

void Solver::solve(MaxCutProblem& problem) {
    requireRewards(config_);
    construct(problem);
    if (searchable(problem)) {
        const int starts = std::max(1, config_.parallel_solver_count);
        if (starts == 1) {
            MaxCutIncrementalScoreCalculator calculator;
            optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem));
            engine.solve(problem, calculator);
        } else {
            searchInParallel(config_, problem, starts);
        }
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::score(problem);
}

void Solver::solve(MaxCutProblem& problem,
                   optimaru::DecomposedObjective<MaxCutProblem>& calculator) {
    requireRewards(config_);
    construct(problem);
    if (searchable(problem)) {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem));
        engine.solve(problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
    problem.scored = true;
}

}  // namespace mc
