#ifndef TA_SOLVER_HPP
#define TA_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "ta/domain.hpp"
#include "optimaru/decomposed_objective.hpp"
#include "optimaru/editor.hpp"

namespace ta
{

// Thin domain-side wrapper around the generic engine in include/optimaru/. It
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
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above
        // every achievable score, so it never fires (termination is by
        // time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * taskCount, 400, 3200).
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each
        // worker receives seed + i and the full wall-clock budget; the
        // best solution wins.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks
            = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruin_recreate = true;
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
        double relocate_weight = 0.4;
        double list_swap_weight = 0.3;
        double two_opt_weight = 0.1;
        double or_opt_weight = 0.1;
        double tail_swap_weight = 0.1;

        std::function<void(std::int64_t, const Score &)> on_new_best;
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
    // is left primed on the final solution, so constraint_totals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallelSolverCount is ignored.
    void solve(
        TaskAssigningSolution &solution,
        optimaru::DecomposedObjective<TaskAssigningSolution> &calculator);

private:
    Config config_;
};

}  // namespace ta

#endif  // TA_SOLVER_HPP
