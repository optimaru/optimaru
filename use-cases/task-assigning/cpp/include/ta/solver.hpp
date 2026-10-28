#ifndef TA_SOLVER_HPP
#define TA_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "ta/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace ta
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<TaskAssigningSolution> (LIST variable: cheapest-insertion
// construction + relocate/swap/2-opt/or-opt/tail-swap local search), with
// the engine's adaptive ruin-and-recreate (ALNS) kicks enabled by default.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // quickstart's application.properties).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above
        // every achievable score, so it never fires (termination is by
        // spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * taskCount, 400, 3200).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): each
        // worker receives randomSeed + i and the full wall-clock budget; the
        // best solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // LIST move mix. Weights are relative; 0 disables a move. Or-opt
        // (segment relocate) and tail swap (2-opt*) are enabled here on top
        // of the engine defaults: both are natural task-queue moves.
        double relocateMoveWeight = 0.4;
        double listSwapMoveWeight = 0.3;
        double twoOptMoveWeight = 0.1;
        double orOptMoveWeight = 0.1;
        double tailSwapMoveWeight = 0.1;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates employees[*].tasks, re-derives the task
    // shadow variables (employee, startTime) and stores the best score found
    // in solution.score. After construction every task is assigned; a task
    // list order is the employee's execution order.
    void solve(TaskAssigningSolution &solution);

    // Solves with a caller-assembled plugin calculator (see
    // ta/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — cheapest-insertion
    // construction, late-acceptance local search over the LIST moves and
    // ALNS kicks. Weights come from the assembly, NOT from the quickstart
    // Bendable defaults. The final solution.score is computed by THIS
    // calculator (decoded back into the Bendable levels) and the calculator
    // is left primed on the final solution, so constraintTotals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallelSolverCount is ignored.
    void solve(
        TaskAssigningSolution &solution,
        optimaru::DecomposedScoreCalculator<TaskAssigningSolution> &calculator);

private:
    Config config_;
};

}  // namespace ta

#endif  // TA_SOLVER_HPP
