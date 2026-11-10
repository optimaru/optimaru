#ifndef MC_SOLVER_HPP
#define MC_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "mc/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace mc
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// builds the start partition (see solve()), wires up the
// incremental score calculator and delegates the search to
// optimaru::Solver<MaxCutProblem> — late acceptance over the built-in assignment
// moves (a change move flips one vertex, a swap move exchanges a side-0 and a
// side-1 vertex), with the engine's adaptive ruin-and-recreate (ALNS) on
// stagnation. Fixed vertices are pinned: no move, swap or ruin touches them.
//
// Exact-size mode (problem.exactSize(): min_partition_size ==
// max_partition_size, both given) keeps the contract "the search never
// changes the side sizes": the solver then forces change_weight = 0,
// swap_weight = 1 and turns ruin-and-recreate (kicks and the as-move),
// descent off, whatever the Config says; with fewer than two movable vertices
// it skips the search. A seed of the wrong size keeps that size (and its hard
// penalty); so does the default start when fixed vertices unbalance it.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 stops before the first step: the solve then
        // returns the start partition unchanged (scored).
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires.
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion (so
        // step_limit = 0 means "no step limit", not "no search").
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * vertex count, 400, 3200).
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts: worker thread i searches its own copy
        // of the start partition with seed + i and the full budget; the
        // best score wins. Only worker 0 calls on_new_best. An exception in a
        // worker (a throwing on_new_best included) stops the other workers and
        // is rethrown by solve(). Ignored by the constraint-assembly overload.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS): a batch of free vertices is unassigned and the
        // engine's greedy recreate puts each back on its better side.
        // Ignored in exact-size mode.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight). 0 (default) disables it; ignored in
        // exact-size mode.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move. With two
        // sides a change move is a flip; a swap move exchanges a side-0 and a
        // side-1 vertex (the sizes stay). Exact-size mode forces 0 / 1.
        double change_weight = 0.8;
        double swap_weight = 0.2;

        // Assignment best-improvement descent strategy weight (engine
        // local_search.descent_weight). 0 (default) = off; ignored in
        // exact-size mode.
        double descent_weight = 0.0;

        std::function<void(std::int64_t, const Score &)> on_new_best;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares (validating the facts), builds the start
    // partition, searches, writes the best partition and problem.score (a
    // full recompute; problem.scored = true), and resets problem.warmStart to
    // false.
    //
    // The start partition: without warmStart every side is
    // reset first; then every unassigned vertex takes its fixed side when it
    // has one, else (index % 2). With warmStart the given sides are kept (a
    // seed that contradicts a fixed side is kept too: the vertex is pinned,
    // so it keeps the hard penalty). An empty graph is scored, not searched.
    void solve(MaxCutProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // mc/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same start partition and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(MaxCutProblem &problem, optimaru::DecomposedObjective<MaxCutProblem> &calculator);

private:
    Config config_;
};

}  // namespace mc

#endif  // MC_SOLVER_HPP
