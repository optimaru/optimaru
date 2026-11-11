#ifndef KS_SOLVER_HPP
#define KS_SOLVER_HPP

#include <utility>

#include "ks/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace ks {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// runs the domain construction (the greedy of construction.hpp), wires up the
// incremental score calculator and delegates the search to
// optimaru::Solver<KnapsackProblem> — late acceptance over the built-in assignment
// moves (a change move is a flip, a swap move trades a selected item for an
// unselected one) and the domain's CoreRepackMove (ks/moves.hpp), with the
// engine's adaptive ruin-and-recreate (ALNS) on stagnation.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(8 * item count, 400, 3200). It is the
    //                          first stage's length: the search restarts with
    //                          a doubled history when it stalls (the engine's
    //                          history_growth)
    //   parallel_solver_count  every start searches its own copy of the
    //                          constructed selection; ignored by the
    //                          constraint-assembly overload
    //   ruin_recreate          a batch of items is dropped to undecided and the
    //                          engine's greedy recreate picks 0 or 1 for each by
    //                          score, which leaves a local optimum of tight
    //                          resource / group instances
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight): a few items are dropped to undecided
        // and greedily re-decided, and the acceptor judges the whole trade.
        // With two values one costs only ~2 * ruinCount evaluations, and it
        // is what lets late acceptance leave a strict local optimum (a 2-for-1
        // exchange at the capacity boundary): on random 8-15 item instances
        // it lifts the brute-force optimum rate from 37/40 to 40/40 at no
        // loss on 50-1000 items. 0 disables it.
        double ruin_recreate_as_move_weight = 0.05;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move. A change
        // move flips one item; a swap move exchanges a selected and an
        // unselected item, which keeps the load near the capacity.
        double change_weight = 0.5;
        double swap_weight = 0.5;

        // Assignment best-improvement descent strategy weight (engine
        // local_search.descent_weight). 0 (default) = off.
        double descent_weight = 0.0;

        // Nearby selection (engine local_search.nearby): biases swap partners
        // toward items of similar weight (distance |weight_a - weight_b|), so
        // a trade changes the load little. probability 0 (default) leaves
        // selection uniform-random.
        double nearby_probability = 0.0;
        int nearby_size = 20;

        // CoreRepackMove (ks/moves.hpp): re-decide a window of the density
        // order around the break item exactly, by dynamic programming over
        // the load. At the capacity boundary the improving exchanges are two
        // for one or larger, which no single flip or swap reaches; without it
        // the 500 to 5,000 item instances of benchmarks/knapsack stay 1 to 6
        // below their optimum.
        bool core_repack_moves = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares (validating the facts), constructs (the
    // greedy; with problem.warmStart only the undecided items), searches,
    // writes the best selection and problem.score (a full recompute), and
    // resets problem.warmStart to false.
    void solve(KnapsackProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // ks/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same construction and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(KnapsackProblem& problem,
               optimaru::DecomposedObjective<KnapsackProblem>& calculator);

private:
    Config config_;
};

}  // namespace ks

#endif  // KS_SOLVER_HPP
