#ifndef PDP_SOLVER_HPP
#define PDP_SOLVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "pdp/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/decider/acceptor/local_search_acceptor_config.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin_recreate_selector.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// The same engine as vehicle-routing, plus the two extension points that a
// pickup-and-delivery problem actually needs:
//
//   * PairRelocateMove (a custom MoveOperator) — moves a request's pickup and
//     dropoff together, and repairs requests whose coupling is currently
//     broken. Without it the engine's element-granular moves have to break a
//     pair to move it and climb back out through a hard-infeasible state,
//     which late acceptance almost never tolerates; the assignment then
//     freezes at whatever the construction heuristic produced.
//
//   * PairRuin (a custom RuinOperator) — ruins whole requests rather than
//     individual stops, so a kick never leaves a pickup stranded without its
//     dropoff.
//
// Both join the engine's adaptive selection alongside the built-ins, so their
// weight is learned rather than fixed.
class Solver
{
public:
    struct Config
    {
        // --- termination ---
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max()};
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // --- construction heuristic ---
        // Tightest time window / largest demand first.
        bool firstFitDecreasing = true;
        // Build the initial solution one complete request at a time with a
        // feasible cheapest insertion. The generic LIST constructor sees
        // individual nodes and strands pairs on larger instances.
        bool pairConstructionHeuristic = true;

        // --- acceptance ---
        optimaru::AcceptorType acceptorType
            = optimaru::AcceptorType::LATE_ACCEPTANCE;
        std::size_t lateAcceptanceSize = 0;  // 0 = auto
        double recordToRecordStartGap = 0.02;
        double recordToRecordEndGap = 0.0;

        // --- move mix ---
        // The built-in element moves are deliberately down-weighted relative
        // to vehicle-routing: on a PDP most single-element moves break a pair
        // and get rejected, so the pair move carries the search. They are not
        // zero because intra-route reordering (2-opt / or-opt) is still how a
        // route's sequence gets polished once the assignment is settled.
        double relocateMoveWeight = 0.15;
        double listSwapMoveWeight = 0.1;
        double twoOptMoveWeight = 0.35;
        double orOptMoveWeight = 0.3;
        double tailSwapMoveWeight = 0.1;
        double interExchangeMoveWeight = 0.0;
        int interExchangeMaxSegment = 3;

        // Whether to register the pair-aware move operator (on by default;
        // turn it off to measure what it is worth).
        bool pairMoves = true;
        bool pairExchangeMoves = true;
        // Exchange is valuable on large route sets but steals too many steps
        // on small instances. Set 0 to force it at every size.
        int pairExchangeMinNodes = 300;
        // Share of pair moves spent repairing a currently-broken request
        // rather than relocating an arbitrary one.
        double pairRepairProbability = 0.7;
        // Share of pair moves that drop a request into the unassigned pool or
        // pull one back out — how the medium level gets optimized at all.
        double pairAssignmentProbability = 0.1;

        // --- adaptive ruin & recreate (ALNS) ---
        bool ruinRecreate = true;
        bool pairRuin = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.40;
        int ruinRecreateMaxRuinCount = 60;
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Search-strategy parity with vehicle-routing. These
        // remain opt-in for PDP because the generic variants manipulate
        // individual nodes; pair-aware move/ruin operators are the defaults.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;
        double descentWeight = 0.0;
        int descentMaxSweeps = 4;
        double routeEliminationWeight = 0.0;
        int routeEliminationMaxSize = 12;
        optimaru::OperatorSelectionType ruinRecreateSelectionType
            = optimaru::OperatorSelectionType::ROULETTE_WHEEL;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{3.0, 2.0, 1.0, 0.5};
        std::int64_t ruinRecreateSelectionSegmentLength = 100;
        double ruinRecreateSelectionAlpha = 0.05;
        bool sisrRuin = true;
        bool routeRuin = true;
        int sisrMaxStringRemovals = 3;
        int sisrMaxStringSize = 12;
        int regretK = 3;
        double blinkRate = 0.01;

        // --- nearby selection ---
        bool drivingTimeNearby = true;
        int drivingTimeNearbySize = 20;
        double drivingTimeNearbyProbability = 0.8;

        // --- engine ---
        std::uint64_t randomSeed = 0;
        int parallelSolverCount = 1;
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;
        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates plan.vehicles[*].nodeIndices and
    // plan.unassignedNodeIndices, and stores the best score in plan.score.
    void solve(PdpRoutePlan &plan);

    // Solves with a caller-assembled plugin calculator (see
    // pdp/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, the pair-aware move and ruin
    // operators included (they read the plan, not the calculator). Weights
    // come from the assembly, NOT from the defaults. The final plan.score is
    // computed by THIS calculator (decoded back into the HardMediumSoft
    // levels) and the calculator is left primed on the final solution, so
    // constraintTotals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallelSolverCount is
    // ignored.
    void solve(PdpRoutePlan &plan,
               optimaru::DecomposedScoreCalculator<PdpRoutePlan> &calculator);

private:
    Config config_;
};

}  // namespace pdp

#endif  // PDP_SOLVER_HPP
