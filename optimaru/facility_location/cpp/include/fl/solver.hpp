#ifndef FL_SOLVER_HPP
#define FL_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "fl/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace fl
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<FacilityLocationProblem>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget.
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires (termination is by time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * consumerCount, 400, 3200), the scaling adopted on the
        // school-timetabling quality bench.
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives seed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks
            = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Reassigning one consumer at a time cannot close a
        // used facility whose consumers must move together; a ruin batch can.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY move (the engine's
        // ruin_recreate.as_move_weight): a small ruin is repaired, scored and
        // judged by the acceptor like any other move — unlike the stagnation
        // kick above, which is rare and unconditional. One as-move costs
        // ~ruinCount * facilityCount score evaluations, so its weight follows
        // the value-range budget-share rule (weight * facilityCount ~ 0.2)
        // measured on the school-timetabling bench. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Nearby selection (engine local_search.nearby): biases swap partners
        // toward geographically close consumers and upgrades the LNS related
        // ruin. The distance meter is the consumers' Euclidean degree
        // distance. probability 0 (default) leaves selection uniform-random.
        double nearbyProbability = 0.0;
        int nearbySize = 20;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability the change move draws the consumer's new
        // facility from its `valueRecommenderKeep` nearest facilities instead
        // of uniformly over all of them. 0 (default) = off.
        double valueRecommenderProbability = 0.0;
        int valueRecommenderKeep = 8;

        // BASIC descent strategy (engine local_search.descent_weight):
        // best-improvement over one nearby consumer pair — all values of
        // both plus their swap — repeated until locally optimal. One
        // application costs ~2 * facilityCount evaluations, so weight by the
        // value range. 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw a caller-registered macro
        // move (CloseFacilityMove) instead of the ordinary mix; the adaptive
        // selector still chooses inside the gate. 1 = unrestricted (engine
        // default).
        double custom_move_probability = 1.0;

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

        std::function<void(std::int64_t, const Score &)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
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
    // and levels come from the assembly (the problem carries none). The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(FacilityLocationProblem &problem,
               optimaru::DecomposedObjective<FacilityLocationProblem>
                   &calculator);

private:
    Config config_;
};

}  // namespace fl

#endif  // FL_SOLVER_HPP
