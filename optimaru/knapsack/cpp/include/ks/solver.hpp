#ifndef KS_SOLVER_HPP
#define KS_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "ks/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace ks
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// runs the domain construction (the greedy of construction.hpp), wires up the
// incremental score calculator and delegates the search to
// optimaru::Solver<KnapsackProblem> — late acceptance over the built-in BASIC
// moves (a change move is a flip, a swap move trades a selected item for an
// unselected one), with the engine's adaptive ruin-and-recreate (ALNS) on
// stagnation.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 stops before the first step: the solve then
        // returns the construction (or a complete seed) unchanged.
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
        // clamp(8 * item count, 400, 3200).
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): worker i
        // searches its own copy of the constructed problem with seed + i
        // and the full budget; the best score wins. Ignored by the
        // constraint-assembly overload.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS): a batch of items is dropped to undecided and the
        // engine's greedy recreate picks 0 or 1 for each by score, which
        // leaves a local optimum of tight resource / group instances.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight): a few items are dropped to undecided
        // and greedily re-decided, and the acceptor judges the whole trade.
        // With two values one costs only ~2 * ruinCount evaluations, and it
        // is what lets late acceptance leave a strict local optimum (a 2-for-1
        // exchange at the capacity boundary): on random 8-15 item instances
        // it lifts the brute-force optimum rate from 37/40 to 40/40 at no
        // loss on 50-1000 items. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.05;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move. A change
        // move flips one item; a swap move exchanges a selected and an
        // unselected item, which keeps the load near the capacity.
        double change_weight = 0.5;
        double swap_weight = 0.5;

        // BASIC best-improvement descent strategy weight (engine
        // local_search.descent_weight). 0 (default) = off.
        double descent_weight = 0.0;

        // Nearby selection (engine local_search.nearby): biases swap partners
        // toward items of similar weight (distance |weight_a - weight_b|), so
        // a trade changes the load little. probability 0 (default) leaves
        // selection uniform-random.
        double nearbyProbability = 0.0;
        int nearbySize = 20;

        std::function<void(std::int64_t, const Score &)> on_new_best;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares (validating the facts), constructs (the
    // greedy; with problem.warmStart only the undecided items), searches,
    // writes the best selection and problem.score (a full recompute), and
    // resets problem.warmStart to false.
    void solve(KnapsackProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // ks/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same construction and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(KnapsackProblem &problem,
               optimaru::DecomposedObjective<KnapsackProblem> &calculator);

private:
    Config config_;
};

}  // namespace ks

#endif  // KS_SOLVER_HPP
