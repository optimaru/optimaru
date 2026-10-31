#ifndef KS_SOLVER_HPP
#define KS_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "ks/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace ks
{

// Thin domain-side wrapper around the generic engine in include/optimaru/: it
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
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires.
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion (so
        // stepCountLimit = 0 means "no step limit", not "no search").
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * item count, 400, 3200).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): worker i
        // searches its own copy of the constructed problem with randomSeed + i
        // and the full budget; the best score wins. Ignored by the
        // constraint-assembly overload.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::EnvironmentMode environmentMode = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS): a batch of items is dropped to undecided and the
        // engine's greedy recreate picks 0 or 1 for each by score, which
        // leaves a local optimum of tight resource / group instances.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruinRecreate.asMoveWeight): a few items are dropped to undecided
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
        double changeMoveWeight = 0.5;
        double swapMoveWeight = 0.5;

        // BASIC best-improvement descent strategy weight (engine
        // localSearch.descentWeight). 0 (default) = off.
        double descentWeight = 0.0;

        // Nearby selection (engine localSearch.nearby): biases swap partners
        // toward items of similar weight (distance |weight_a - weight_b|), so
        // a trade changes the load little. probability 0 (default) leaves
        // selection uniform-random.
        double nearbyProbability = 0.0;
        int nearbySize = 20;

        std::function<void(std::int64_t, const Score &)> onNewBest;
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
    // primed on the final solution, so constraintTotals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(KnapsackProblem &problem,
               optimaru::DecomposedScoreCalculator<KnapsackProblem> &calculator);

private:
    Config config_;
};

}  // namespace ks

#endif  // KS_SOLVER_HPP
