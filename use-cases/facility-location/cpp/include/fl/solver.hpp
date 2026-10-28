#ifndef FL_SOLVER_HPP
#define FL_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "fl/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace fl
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<FacilityLocationProblem>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves.
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
        // clamp(8 * consumerCount, 400, 3200), the scaling adopted on the
        // school-timetabling quality bench.
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
        // engine's ALNS). Reassigning one consumer at a time cannot close a
        // used facility whose consumers must move together; a ruin batch can.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY move (the engine's
        // ruinRecreate.asMoveWeight): a small ruin is repaired, scored and
        // judged by the acceptor like any other move — unlike the stagnation
        // kick above, which is rare and unconditional. One as-move costs
        // ~ruinCount * facilityCount score evaluations, so its weight follows
        // the value-range budget-share rule (weight * facilityCount ~ 0.2)
        // measured on the school-timetabling bench. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Nearby selection (engine localSearch.nearby): biases swap partners
        // toward geographically close consumers and upgrades the LNS related
        // ruin. The distance meter is the consumers' Euclidean degree
        // distance. probability 0 (default) leaves selection uniform-random.
        double nearbyProbability = 0.0;
        int nearbySize = 20;

        // Guided change-move targets (engine localSearch.valueRecommender):
        // with this probability the change move draws the consumer's new
        // facility from its `valueRecommenderKeep` nearest facilities instead
        // of uniformly over all of them. 0 (default) = off.
        double valueRecommenderProbability = 0.0;
        int valueRecommenderKeep = 8;

        // BASIC descent strategy (engine localSearch.descentWeight):
        // best-improvement over one nearby consumer pair — all values of
        // both plus their swap — repeated until locally optimal. One
        // application costs ~2 * facilityCount evaluations, so weight by the
        // value range. 0 (default) = off.
        double descentWeight = 0.0;

        // Upper gate on how often a step may draw a caller-registered macro
        // move (CloseFacilityMove) instead of the ordinary mix; the adaptive
        // selector still chooses inside the gate. 1 = unrestricted (engine
        // default).
        double customMoveProbability = 1.0;

        // CloseFacilityMove: empty one used facility in a single move (see
        // solver.cpp). facilitySetupCost is a step function no single-consumer
        // move can cross, so without this the search converges in a fraction
        // of a second and then stops improving.
        bool closeFacilityMoves = true;
        // How far down a consumer's distance-ordered facility list the move
        // looks for a new home.
        int closeFacilityKeep = 8;
        // Probability that the emptied facility's consumers migrate together
        // to one alternative site instead of being redistributed over the
        // facilities that are already open.
        double closeFacilityMigrateProbability = 0.25;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates consumers[*].facility and stores the best
    // score found in problem.score.
    void solve(FacilityLocationProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // fl/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — construction heuristic,
    // late-acceptance local search, ALNS kicks and CloseFacilityMove. Weights
    // come from the assembly, NOT from the problem's weight fields. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraintTotals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(FacilityLocationProblem &problem,
               optimaru::DecomposedScoreCalculator<FacilityLocationProblem>
                   &calculator);

private:
    Config config_;
};

}  // namespace fl

#endif  // FL_SOLVER_HPP
