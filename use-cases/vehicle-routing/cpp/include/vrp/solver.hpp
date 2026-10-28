#ifndef VRP_SOLVER_HPP
#define VRP_SOLVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/decider/acceptor/local_search_acceptor_config.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin_recreate_selector.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "vrp/domain.hpp"

namespace vrp
{

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++ analog
// of the quickstart building a SolverFactory from a SolverConfig.
//
// This replaces the hand-written Late Acceptance loop the port shipped with.
// The engine gives it, in Timefold's phase structure:
//
//   1. Construction heuristic — cheapest insertion of every visit.
//   2. Local search — one candidate move per step (relocate / swap / 2-opt /
//      or-opt / tail swap), judged by the configured acceptor.
//   3. Adaptive ruin & recreate (ALNS) — when the local search stagnates, an
//      adaptively selected ruin operator tears out part of the solution and a
//      regret-k repair rebuilds it.
//
// The ALNS machinery uses four
// operator-selection schemes, configurable acceptance criteria, and the ruin
// operators that matter on routing problems — SISR substring removal and
// whole-route removal alongside random / related / worst.
//
// Unassigned visits: the quickstart's allowsUnassignedValues is modelled with
// the plan's virtual unassigned pool (see VehicleRoutePlan). solve() fills the
// pool for you, so a plan built with visits simply left out of every route is
// accepted as-is.
class Solver
{
public:
    struct Config
    {
        // --- termination -------------------------------------------------
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // Java application.properties).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit, mirroring the test
        // profile's best-score-limit=0hard/0medium/*soft. The default is set
        // above every achievable score (the optimum is 0/0/0), so it never
        // fires — termination is by spentLimit. To stop as soon as a feasible,
        // fully-assigned solution is found, set {0, 0, min}.
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // --- construction heuristic ---------------------------------------
        // First Fit Decreasing: initialize the tightest-window, largest-demand
        // visits first. Plain First Fit (input order) with false.
        bool firstFitDecreasing = true;

        // --- acceptance ---------------------------------------------------
        optimaru::AcceptorType acceptorType
            = optimaru::AcceptorType::LATE_ACCEPTANCE;
        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(60000 / visitCount, 100, 400) — the horizon SHRINKS as the
        // instance grows, keeping it roughly constant in wall-time as the
        // per-step cost rises (measured on CVRPLIB Set X; see solver.cpp).
        std::size_t lateAcceptanceSize = 0;
        // RECORD_TO_RECORD_TRAVEL: the acceptable gap below the best score,
        // as a fraction of the initial objective, shrinking start -> end.
        double recordToRecordStartGap = 0.02;
        double recordToRecordEndGap = 0.0;

        // --- move mix -----------------------------------------------------
        double relocateMoveWeight = 0.4;
        double listSwapMoveWeight = 0.2;
        double twoOptMoveWeight = 0.15;
        double orOptMoveWeight = 0.15;
        // 2-opt* (tail swap): the classic inter-route VRP move. Worth real
        // weight here — with a per-vehicle capacity and per-visit time window,
        // exchanging two route tails is the cheapest way to rebalance load
        // between vehicles without walking through infeasible intermediates.
        double tailSwapMoveWeight = 0.1;
        // CROSS-exchange: swap a run of visits in one route with a run in
        // another (see MoveSelectorConfig).
        //
        // OFF by default. The move rebalances two routes by swapping segments
        // without requiring an over-capacity intermediate state.
        double interExchangeMoveWeight = 0.0;
        int interExchangeMaxSegment = 3;

        // --- adaptive ruin & recreate (ALNS) ------------------------------
        // The stagnation KICK (ruin 10-30%, repair, make it the incumbent
        // unconditionally). OFF by default: on a tightly loaded fleet the
        // kicked solution is far below the best and the search never finds
        // its way back, so the best stops moving at the first kick. CVRPLIB
        // Set X, 60s, seeds 0/1: with the kick X-n289-k60 finds its last
        // improvement at 5-17s and ends 9.3-12.3% above BKS; without it the
        // search improves until the clock runs out on every instance and
        // ends 6.4/8.7% (X-n289), 17.0/17.9% (X-n895, was 22%), with
        // X-n153/491/701 unchanged or better. Destroy-and-repair still runs
        // as an acceptor-judged move (ruinRecreateAsMoveWeight below).
        bool ruinRecreate = false;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Search strategies registered as move operators from
        // include/optimaru/impl/localsearch/strategy/.
        // The weights are deliberately small: all three cost far more than one
        // sampled move (a descent sweep scans a whole list pair, the other two
        // run a repair), so they earn their keep at a few percent of steps and
        // starve the ordinary move mix above that. LocalSearchConfig records
        // why these are configured rather than adaptively learned — it was
        // tried, and it is the selection schemes that are not ready for it.
        double ruinRecreateAsMoveWeight = 0.05;
        int ruinRecreateAsMoveMaxRuin = 12;
        // PyVRP-style granular intensification: enumerate the bounded
        // Exchange/2-opt/2-opt*/CROSS neighbourhood around a nearby route
        // pair. This is more expensive than a sampled move, so keep the
        // strategy at a measured small share of the search budget.
        double descentWeight = 0.03;
        int descentMaxSweeps = 4;
        bool granularDescent = true;
        int granularDescentMaxSegment = 2;
        int granularDescentCandidateRoutes = 4;
        // Empty the smallest route into the others in one move. The (1,0)
        // relocations above cannot do this on their own — the payoff is the two
        // depot legs that only disappear when the LAST visit leaves, so every
        // intermediate step is uphill. 0.10 measured best; 0.02 gives +8.6%/36
        // and the difference between them is small (see the repair note in
        // core's greedy_repair.hpp about nearby anchors during a route ruin).
        double routeEliminationWeight = 0.10;
        int routeEliminationMaxSize = 12;
        // Ejection chain: insert an unassigned visit (or take an infeasible
        // best-soft relocation) and repair the capacity/time-window breach
        // with strictly hard-improving relocations and swaps, as one move.
        // It is what reaches the slack shifts a tightly loaded fleet needs —
        // each single swap is uphill, so late acceptance never chains them.
        // 0.05 measured on CVRPLIB Set X (60s, seeds 0-1, one instance per
        // Hexaly bucket): X-n895-k37 goes from 1-2 unassigned visits to fully
        // assigned, X-n289-k60 from 15.8/14.4% to 9.7/9.3% gap, and X-n153
        // (3.1/2.6 -> 1.5/2.5), X-n491 (5.1/4.7 -> 4.8/3.8) and X-n701
        // (5.1/4.4 -> 4.3/4.4) improve too. Measured with the stagnation kick
        // still on; the final numbers with it off are under ruinRecreate
        // above and in benchmarks/vehicle-routing/README.md.
        double ejectionChainWeight = 0.05;
        int ejectionChainMaxSteps = 24;
        int ejectionChainCandidateLists = 4;
        // Score evaluations one chain may spend before it gives up.
        std::int64_t ejectionChainMaxEvaluations = 50000;

        // Iterated local search as a move: ruin a few visits, repair, then
        // descend (relocate / swap / 2-opt / 2-opt* / Or-opt / CROSS around
        // each visit's nearby neighbours) until the touched routes are locally
        // optimal, and let the acceptor judge the result. 0 disables it. It
        // runs on move previews, which every VRP path serves: O(1) from
        // prefix sums on the monolithic path when no visit waits for its
        // window, one walk of the candidate route when visits wait
        // (incremental_calculator.hpp), and a per-component re-cost of the
        // candidate routes on the plugin path (DecomposedScoreCalculator).
        //
        // Defaults measured on CVRPLIB X-n289-k60 / X-n895-k37 (10s, seeds
        // 0-1, mean gap to BKS): off 12.5%; weight 0.05 with ruin 5-15 and
        // Or-opt up to 3: 7.3%; Or-opt off (segment 1): 6.0%; nearby 25:
        // 5.4%; ruin 2-8: 4.9%. At 60s that setting reaches 2.5 / 2.6%
        // (X-n289) and 4.6 / 4.7% (X-n895), from 6.0 / 8.1% and 9.7 / 11.7%.
        double iteratedLocalSearchWeight = 0.05;
        int iteratedLocalSearchMinRuin = 2;
        int iteratedLocalSearchMaxRuin = 8;
        int iteratedLocalSearchMaxSegment = 1;
        std::int64_t iteratedLocalSearchMaxEvaluations = 5000000;

        // Which adaptive scheme weights the ruin operators.
        optimaru::OperatorSelectionType ruinRecreateSelectionType
            = optimaru::OperatorSelectionType::ROULETTE_WHEEL;
        double ruinRecreateOperatorDecay = 0.8;
        // Rewards indexed by outcome: NEW_BEST / BETTER / ACCEPTED / REJECTED.
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreateSelectionSegmentLength = 100;
        double ruinRecreateSelectionAlpha = 0.05;

        // Ruin operators joining the always-on random / related / worst trio.
        bool sisrRuin = true;
        bool routeRuin = true;
        int sisrMaxStringRemovals = 3;
        int sisrMaxStringSize = 12;

        // Repair: regret-k insertion (>= 2) and SISR-style blinking.
        int regretK = 3;
        double blinkRate = 0.01;

        // --- nearby selection ---------------------------------------------
        // Driving-time nearby neighborhoods over the visits: biases move
        // partners and repair positions toward visits that are close in
        // driving time — the natural neighborhood for a travel-time objective,
        // and what makes the related ruin and the SISR ruin meaningful.
        // The ejection chain and the iterated local search need it: with it
        // off they decline every draw and only burn their share of the steps.
        bool drivingTimeNearby = true;
        // PyVRP's granular neighbourhood defaults to 50 nearest customers;
        // keep the same candidate horizon for route-pair intensification.
        // 25 (from 50) with the iterated local search: half the candidates
        // per visit for the descent and the repair, which bought more than
        // the lost reach on Set X (see iteratedLocalSearchWeight).
        int drivingTimeNearbySize = 25;
        double drivingTimeNearbyProbability = 1.0;

        // --- engine -------------------------------------------------------
        std::uint64_t randomSeed = 0;
        // Independent parallel starts: each worker receives randomSeed + i and
        // the full wall-clock budget; the best final score wins.
        int parallelSolverCount = 1;
        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; far slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Optional progress callback: (steps, bestScore).
        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates plan.vehicles[*].visitIndices and
    // plan.unassignedVisitIndices, and stores the best score found in
    // plan.score.
    void solve(VehicleRoutePlan &plan);

    // Solves with a caller-assembled plugin calculator (see
    // vrp/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, driving-time nearby selection
    // included. Weights come from the assembly, NOT from the quickstart level
    // defaults. The final plan.score is computed by THIS calculator (decoded
    // back into the HardMediumSoft levels) and the calculator is left primed
    // on the final solution, so constraintTotals() explains exactly that
    // score. Single-start: the caller-owned calculator cannot be cloned, so
    // parallelSolverCount is ignored.
    void solve(VehicleRoutePlan &plan,
               optimaru::DecomposedScoreCalculator<VehicleRoutePlan> &calculator);

private:
    Config config_;
};

}  // namespace vrp

#endif  // VRP_SOLVER_HPP
