#ifndef ALB_SOLVER_HPP
#define ALB_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "alb/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace alb
{

// Thin domain-side wrapper around the generic engine in include/optimaru/: it
// runs the domain construction (the ported 0.1.5 first fit, see
// construction.hpp), freezes the station value range, wires up the
// incremental score calculator and the domain's own move (CloseStationMove)
// and value recommender, and delegates the search to
// optimaru::Solver<AssemblyLineBalancingProblem> (late acceptance + adaptive
// ruin-and-recreate on top of the built-in change / swap moves).
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
        // clamp(8 * task count, 400, 3200).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts: worker i solves its own copy of the
        // constructed problem with randomSeed + i and the full budget; the
        // best score wins. Ignored by the constraint-assembly overload.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::EnvironmentMode environmentMode = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS).
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruinRecreate.asMoveWeight); one costs ~ruinCount * station count
        // evaluations. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move. Swaps move
        // load across a station boundary without opening a station.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Guided change-move targets (engine localSearch.valueRecommender):
        // with this probability a change move draws the task's new station
        // uniformly from its current window — [latest predecessor station,
        // earliest successor station] intersected with its own
        // [min_station, max_station] (and max_stations) — instead of
        // uniformly over all stations. 0 = off.
        double valueRecommenderProbability = 0.5;

        // BASIC best-improvement descent strategy weight (engine
        // localSearch.descentWeight); ~2 * station count evaluations per
        // application. 0 (default) = off.
        double descentWeight = 0.0;

        // Upper gate on how often a step may draw CloseStationMove instead
        // of the ordinary mix; the adaptive selector still chooses inside the
        // gate. 1 = unrestricted (engine default).
        double customMoveProbability = 1.0;

        // CloseStationMove: empty one used station in a single move (see
        // solver.cpp). usedStationsCost is a step function no single-task
        // move can cross.
        bool closeStationMoves = true;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares the problem, constructs (clearing every
    // station first unless problem.warmStart), searches, writes the best
    // stations and problem.score (a full recompute), and resets
    // problem.warmStart to false. Throws std::invalid_argument on invalid
    // facts or a cyclic precedence graph.
    void solve(AssemblyLineBalancingProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // alb/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same construction and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraintTotals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallelSolverCount is ignored.
    void solve(AssemblyLineBalancingProblem &problem,
               optimaru::DecomposedScoreCalculator<AssemblyLineBalancingProblem> &calculator);

private:
    Config config_;
};

}  // namespace alb

#endif  // ALB_SOLVER_HPP
