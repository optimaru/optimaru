#ifndef MC_SOLVER_HPP
#define MC_SOLVER_HPP

#include <utility>

#include "mc/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace mc {

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
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit 0           the solve returns the start partition unchanged
    //                          (scored)
    //   history_length 0       clamp(8 * vertex count, 400, 3200). It is the
    //                          first stage's length: the search restarts with
    //                          a doubled history when it stalls (the engine's
    //                          history_growth)
    //   parallel_solver_count  every start searches its own copy of the start
    //                          partition; a start that throws (a throwing
    //                          on_new_best included) stops the others and is
    //                          rethrown by solve(). Ignored by the
    //                          constraint-assembly overload
    //   ruin_recreate          a batch of free vertices is unassigned and the
    //                          engine's greedy recreate puts each back on its
    //                          better side. Ignored in exact-size mode
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight). 0 (default) disables it; ignored in
        // exact-size mode.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move. With two
        // sides a change move is a flip; a swap move exchanges a side-0 and a
        // side-1 vertex (the sizes stay). Exact-size mode forces 0 / 1.
        double change_weight = 0.8;
        double swap_weight = 0.2;

        // Assignment best-improvement descent strategy weight (engine
        // local_search.descent_weight). 0 (default) = off; ignored in
        // exact-size mode.
        double descent_weight = 0.0;
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
    void solve(MaxCutProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // mc/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same start partition and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(MaxCutProblem& problem, optimaru::DecomposedObjective<MaxCutProblem>& calculator);

private:
    Config config_;
};

}  // namespace mc

#endif  // MC_SOLVER_HPP
