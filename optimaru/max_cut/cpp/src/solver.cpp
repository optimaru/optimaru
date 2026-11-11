#include "mc/solver.hpp"

#include <algorithm>

#include "mc/incremental_calculator.hpp"
#include "mc/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace mc {

namespace {

// The engine configuration of one search, shared by both solve() overloads
// and every parallel start: late-acceptance local search over the built-in
// assignment moves, plus the ALNS kicks. The engine's construction heuristic finds
// nothing left to place: the start partition assigned every vertex.
optimaru::Settings engineConfigOf(const Solver::Config& config, const MaxCutProblem& problem) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.vertexIds.size(), 400, 3200);
    // Restart late acceptance with a doubled history when the search stalls
    // (see AcceptanceSettings::history_growth): measured on every instance of
    // benchmarks/max-cut, none worse at 30 s on three seeds.
    engineConfig.search.acceptance.history_growth = 2.0;
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;

    if (problem.exactSize()) {
        // Swaps only: every other move (a flip, a ruin's unassign-and-recreate,
        // a descent's change) could change a side size.
        engineConfig.search.moves.change_weight = 0.0;
        engineConfig.search.moves.swap_weight = 1.0;
        engineConfig.search.descent_weight = 0.0;
        engineConfig.search.ruin_recreate.enabled = false;
        engineConfig.search.ruin_recreate.as_move_weight = 0.0;
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

// One start, scored by the monolithic incremental calculator. It leaves the
// engine's best score in problem.score: the parallel starts are compared by
// it, and solve() stores the full recompute over it afterwards.
void searchOneStart(const Solver::Config& config, MaxCutProblem& problem) {
    MaxCutIncrementalScoreCalculator calculator;
    optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config, problem));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    problem.score = engine.solve(problem, calculator);
}

}  // namespace

void Solver::solve(MaxCutProblem& problem) {
    optimaru::check_config(config_);
    construct(problem);
    if (searchable(problem)) {
        // Independent parallel starts: start i searches its own copy of the
        // start partition with seed + i and the full budget (only start 0
        // reports on_new_best); the best engine score wins, the lowest start
        // on a tie. A start that throws (a throwing on_new_best, a
        // ScoreChecks::Full ScoreMismatchError, a bad_alloc) stops the others
        // early and is rethrown here once every start has joined, instead of
        // terminating the process.
        optimaru::solve_parallel_starts(config_, problem,
                                        [](const Config& config, MaxCutProblem& candidate, int) {
                                            searchOneStart(config, candidate);
                                        });
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::score(problem);
}

void Solver::solve(MaxCutProblem& problem,
                   optimaru::DecomposedObjective<MaxCutProblem>& calculator) {
    optimaru::check_config(config_);
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
