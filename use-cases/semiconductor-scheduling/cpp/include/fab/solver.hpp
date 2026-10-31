#ifndef FAB_SOLVER_HPP
#define FAB_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "fab/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/decider/acceptor/local_search_acceptor_config.hpp"

namespace fab
{

// Thin domain-side wrapper around the generic engine in include/optimaru/:
// it builds the search state (the default or the seeded (rank, machine)
// values), wires the incremental calculator, the guided change move and the
// priority-swap move, and delegates to optimaru::Solver.
//
// What solve() does, in order:
//   1. values: warmStart ? the seed state of `assignments` (seedState) : the
//      default machines (defaultChoices) at identity ranks. A warm start that
//      assigns every operation is complete: while every row holds those
//      values, the score is that of `assignments` AS SUPPLIED (never decoded).
//   2. the engine: simulated annealing over change moves (steered by the
//      value recommender: a machine switch or a small priority shift) and
//      priority swaps; parallelSolverCount independent starts (seed + i), the strictly
//      best wins, so the start is never lost.
//   3. the result: assignments = the seed as supplied if the best values are
//      the seed values, else their decode (batch ids included); score,
//      makespan and totals in data mode; warmStart = false.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 stops right after construction (the start is
        // returned as is; the zero-budget idiom of the tests).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit (default: never).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        std::uint64_t randomSeed = 0;
        // Independent starts, one thread each (seed + i); the best wins. The
        // constraints overload runs a single start (see there).
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::EnvironmentMode environmentMode = optimaru::EnvironmentMode::NON_ASSERT;

        // Acceptor: simulated annealing (the 0.1.x search was one too). With a
        // (0, 0) starting temperature (the default) it is auto: hard 0 (a
        // hard-worse move is never taken) and soft
        // simulatedAnnealingTemperatureFraction * |the start's soft score|
        // (at least 1), cooling linearly over the spent limit. Measured on the
        // benchmark instances and the web demo, 1% beat late acceptance (which
        // stalls within a second on these plateaus) and 0.5% / 2% / 5%.
        // lateAcceptanceSize (LATE_ACCEPTANCE) 0 = auto: clamp(16 * rows, 400, 3200).
        optimaru::AcceptorType acceptorType = optimaru::AcceptorType::SIMULATED_ANNEALING;
        Score simulatedAnnealingStartingTemperature{0, 0.0};
        double simulatedAnnealingTemperatureFraction = 0.01;
        std::size_t lateAcceptanceSize = 0;

        // Moves. The change move draws its value from the recommender: with
        // machineMoveProbability another machine for the row (rank kept),
        // otherwise a small priority shift (machine kept, rank +- 1..8);
        // `changeMoveWeight` / `swapMoveWeight` are the engine's BASIC mix
        // (the built-in swap would trade whole values between unrelated
        // rows, so it is off). The priority swap exchanges the ranks of two
        // rows of different lots, both machines kept; customMoveProbability
        // gates how often a step may draw it.
        double changeMoveWeight = 1.0;
        double swapMoveWeight = 0.0;
        double machineMoveProbability = 0.55;
        bool prioritySwapMoves = true;
        double customMoveProbability = 0.45;

        // Ruin-and-recreate (the engine's ALNS). Off: a recreate evaluates
        // every value of every ruined row, each one a decode.
        bool ruinRecreate = false;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        std::function<void(std::int64_t, const Score &)> onNewBest;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stopFlag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place (see the class comment): fills assignments, score,
    // makespan, totalWeightedTardiness, totalCycleTime; warmStart = false.
    // If the search throws (for example the decoder giving up), the schedule
    // is back in data mode with its assignments untouched, and the error
    // propagates. Both overloads.
    void solve(SemiconductorSchedule &schedule);

    // The same search scored by a caller-assembled plugin calculator (see
    // fab/constraints.hpp). The final score is computed by THIS calculator in
    // data mode, and the calculator is left primed on the final assignments,
    // so constraintTotals() explains exactly that score; makespan and the
    // totals come from every constraint, as always. Single start: the
    // caller-owned calculator cannot be cloned, so parallelSolverCount is
    // ignored.
    void solve(SemiconductorSchedule &schedule,
               optimaru::DecomposedScoreCalculator<SemiconductorSchedule> &calculator);

private:
    // solve(schedule) after beginSearch(): the starts and endSearch().
    void searchFromBegun(SemiconductorSchedule &schedule);
    void runSingle(SemiconductorSchedule &schedule,
                   optimaru::IncrementalScoreCalculator<SemiconductorSchedule> &calculator) const;

    Config config_;
};

// --- the search state, for the tests and the bindings ---------------------

// Step 1 of Solver::solve: fills rows[*].value (and the seed fields) from
// warmStart / assignments, and sets searching = true.
void beginSearch(SemiconductorSchedule &schedule);
// Step 3 of Solver::solve: assignments from the current values (the seed as
// supplied, or the decode), searching = false, warmStart = false, and the
// default data-mode score stored.
void endSearch(SemiconductorSchedule &schedule);

}  // namespace fab

#endif  // FAB_SOLVER_HPP
