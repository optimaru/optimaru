#ifndef OP_SOLVER_HPP
#define OP_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "op/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace op
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of the quickstart building a SolverFactory from a SolverConfig
// (following the school-timetabling Solver pattern,
// LIST-kind edition). On top of the built-in list move mix (relocate / swap /
// 2-opt / or-opt / tail swap) and the engine's adaptive ruin-and-recreate
// (ALNS, on by default) it registers a warehouse-distance nearby meter, which
// biases move partners toward picks that are physically close — the natural
// neighborhood for a travel-distance objective.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // quickstart's application.properties).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires (termination is by spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(4 * pickTaskCount, 200, 1600).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives randomSeed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS) — lets the trolley assignment escape the local
        // optima that pure route polishing locks into. The trigger is high
        // on purpose: the consolidation move reaches a strong record within
        // ~10^3 steps, and each kick unconditionally replaces the incumbent
        // and resets the acceptor, so an early kick cadence keeps
        // interrupting the slow travel-distance descent that needs ~10^6
        // uninterrupted steps (measured on the 4x instance: trigger 5000
        // stalled at the step-703 record; 50000 kept both the kicks' order
        // diversification and the full descent).
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 50000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // LIST move mix. Weights are relative; 0 disables a move. Or-opt
        // (short-segment relocate) and tail swap (2-opt*) are on: both are
        // classic routing moves and this is a routing-shaped problem.
        double relocateMoveWeight = 0.40;
        double listSwapMoveWeight = 0.25;
        double twoOptMoveWeight = 0.15;
        double orOptMoveWeight = 0.10;
        double tailSwapMoveWeight = 0.10;

        // Warehouse-distance nearby selection: move partners are drawn from
        // the physically closest picks with this probability (the engine's
        // nearby neighborhoods; also drives the LNS related ruin). Size 24
        // measured best across the demo and the 2x/4x bench instances — the
        // wider neighborhood keeps enough cross-trolley partners for the
        // split-structure improvements that a tight 12-pick one misses.
        bool distanceNearby = true;
        double distanceNearbyProbability = 0.5;
        int distanceNearbySize = 24;

        // Custom order-level move operator (adaptive selection alongside the
        // built-in list moves). The order-split penalty (1000 per extra
        // trolley) dominates the soft level, so every cross-trolley move at
        // element granularity is a near-certain reject; this operator works
        // at whole-order granularity instead — gather a split order onto one
        // trolley, and once none is split, relocate or swap whole orders
        // between trolleys. See OrderConsolidateMove in solver.cpp.
        bool orderConsolidateMoves = true;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates trolleys[*].pickTaskIndices, refreshes the
    // pick tasks' shadow variables and stores the best score found in
    // solution.score.
    void solve(OrderPickingSolution &solution);

    // Solves with a caller-assembled plugin calculator (see
    // op/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, distance-nearby selection
    // included. Weights come from the assembly, NOT from the quickstart
    // defaults. The final solution.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraintTotals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallelSolverCount (the
    // caller-owned calculator cannot be cloned across workers) and
    // orderConsolidateMoves (that operator reads the monolithic calculator's
    // live per-(trolley, order) volume table).
    void solve(
        OrderPickingSolution &solution,
        optimaru::DecomposedScoreCalculator<OrderPickingSolution> &calculator);

private:
    Config config_;
};

}  // namespace op

#endif  // OP_SOLVER_HPP
