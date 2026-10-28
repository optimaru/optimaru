#ifndef PJS_SOLVER_HPP
#define PJS_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "pjs/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace pjs
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<ProjectJobSchedule>, with the engine's adaptive
// ruin-and-recreate (ALNS) kicks enabled by default.
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
                             std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<std::int64_t>::max()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(16 * allocationCount, 400, 3200).
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

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Guided change moves for the composite (delay x executionMode)
        // encoding. A uniform draw over the value grid is delay-heavy: it
        // scatters delays uniformly over [0, delayRangeSize) although good
        // schedules keep almost every delay tiny, so most uniform change
        // moves are wasted. With this probability a change move instead
        // keeps the delay near the current one (+- small geometric delta,
        // sometimes delay-preserving to switch only the execution mode) with
        // a fresh random mode slot; the rest of the draws stay uniform for
        // diversity. 0 disables the guidance entirely.
        double nearCurrentDelayProbability = 0.85;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates allocations[*].modeDelay, refreshes the
    // derived dates and stores the best score found in schedule.score.
    // Pinned allocations (all SOURCE/SINK ones plus user-pinned STANDARD
    // ones) keep their assignment; a pinned allocation that arrives
    // unassigned is fixed at (first execution mode, delay 0) first — the
    // Java DemoDataGenerator's convention for sources and sinks.
    void solve(ProjectJobSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // pjs/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, delay-guided value selection
    // included. Weights come from the assembly, NOT from the quickstart level
    // defaults. The final schedule.score is computed by THIS calculator
    // (decoded back into the HardMediumSoft levels) and the calculator is
    // left primed on the final solution, so constraintTotals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallelSolverCount is ignored.
    void
    solve(ProjectJobSchedule &schedule,
          optimaru::DecomposedScoreCalculator<ProjectJobSchedule> &calculator);

private:
    Config config_;
};

}  // namespace pjs

#endif  // PJS_SOLVER_HPP
