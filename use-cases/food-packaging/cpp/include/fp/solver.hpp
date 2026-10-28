#ifndef FP_SOLVER_HPP
#define FP_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "fp/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace fp
{

// Java-CH stand-in for the Line.operator basic variable: every line without
// an operator gets the least-used operator so far (ties: lowest index) —
// injective whenever operators >= lines. No-op when there are no operators.
void assignMissingOperators(PackagingSchedule &schedule);

// One hill-climbing pass over the Line.operator assignment with the job
// lists FIXED: repeatedly moves single lines to the operator that most
// reduces the total same-operator cleaning-window overlap (the only
// constraint that reads operators), until no move improves. Returns whether
// anything changed. Exact when operators >= lines (any injective assignment
// zeroes the conflict).
bool improveOperatorAssignment(PackagingSchedule &schedule);

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates the job-list
// variable to optimaru::Solver<PackagingSchedule> with the engine's LIST
// moves (relocate/swap/2-opt/or-opt/tail-swap) and adaptive
// ruin-and-recreate (ALNS) enabled.
//
// The Java model has a SECOND genuine variable, Line.operator, which the
// single-variable-model engine cannot search jointly. The wrapper reproduces
// its semantics with an alternating scheme: assignMissingOperators seeds the
// assignment, then (only when lines actually share operators — otherwise the
// conflict constraint is identically 0 and one engine run suffices) it
// alternates engine legs over the job lists with improveOperatorAssignment
// steps, splitting the time budget. The score semantics are identical to
// Java's; only the traversal of the operator dimension differs (caveat: for
// operators < lines the operator assignment is optimized between legs, not
// jointly move-by-move).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // quickstart's application.properties). Shared across all
        // engine legs.
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit, as the ENGINE score
        // (hard = Java hard; soft = medium * kMediumUnit + soft). E.g.
        // {0, -1e18} stops at the first hard-feasible solution. The default
        // never fires.
        optimaru::HardSoftScore bestScoreLimit{
            std::numeric_limits<std::int64_t>::max(),
            std::numeric_limits<double>::infinity()};
        // Optional early exits per engine leg; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * unpinned job count, 400, 3200).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start) per leg:
        // each worker receives randomSeed + i and the leg's full budget; the
        // best solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Operator-alternation rounds. 0 = auto: 1 when no two lines share
        // an operator after seeding (the conflict constraint can never
        // fire), else 5 (adopted R4: 5 legs beat 3 and 2 on the shared-
        // operator bench).
        int operatorRounds = 0;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Auto-disabled for tiny instances (< 4 movable
        // jobs).
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
        // of the engine defaults: sequences of consecutive jobs matter in
        // this domain (cleanup chains). Adopted (R2): the relocate-heavy mix
        // below beat the old .4/.25/.15/.1/.1 on every instance (demo +0.2%,
        // 3x +0.35%, shared-operator +32% engine-soft; 6 seeds, hard never
        // worse) — relocate both feeds the nearby bias (cheap-changeover
        // insertions) and is the only move that empties/fills the unassigned
        // pool; swap-heavy and segment-heavy mixes lost on the shared
        // instance.
        double relocateMoveWeight = 0.55;
        double listSwapMoveWeight = 0.25;
        double twoOptMoveWeight = 0.08;
        double orOptMoveWeight = 0.07;
        double tailSwapMoveWeight = 0.05;

        // Nearby selection over the cleanup matrix: distance(a, b) =
        // cleanup(a -> b) + cleanup(b -> a), tie-broken by |idealEnd
        // difference|. Same product = 0, same product family ~ 2x10 minutes,
        // unrelated ~ 60-120 minutes, so swap partners / relocate insertion
        // points are biased toward cheap-changeover neighbors (same-family
        // adjacency); the same lists drive the LNS related ruin and the
        // nearby-restricted repair. probability 0 disables (uniform
        // selection). Adopted (R1): 10s/1-thread interleaved A/B vs the
        // no-nearby baseline on demo (5x100) / 3x-scaled (10x300) / shared-
        // operator (10x300, 5 ops) gave +0.1% / +1.9% / +38.8% engine-soft
        // (6 seeds, hard never worse); size 10 beat 20 and 40, probability
        // 0.8 beat 0.5.
        double nearbyProbability = 0.8;
        int nearbySize = 10;

        // New-best callback with the decoded HardMediumSoft score. With
        // multiple legs/starts, step counts restart per leg and only worker
        // 0 reports.
        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates lines[*].jobs / unassignedJobs (and
    // lines[*].operatorIndex where sharing forces alternation), refreshes
    // every job's shadow variables and stores the best score found in
    // schedule.score. Pinned jobs keep their exact position (or stay
    // unassigned).
    void solve(PackagingSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // fp/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the quickstart
    // level defaults. The final schedule.score is computed by THIS calculator
    // (decoded back into the HardMediumSoft levels) and the calculator is
    // left primed on the final solution, so constraintTotals() explains
    // exactly that score.
    //
    // Two knobs do not apply on this path: parallelSolverCount and
    // operatorRounds. Both need a fresh calculator per start / per leg, and a
    // caller-owned assembly cannot be cloned; the plugin path runs one leg,
    // one start, with the same move mix, nearby selection and ALNS kicks.
    void
    solve(PackagingSchedule &schedule,
          optimaru::DecomposedScoreCalculator<PackagingSchedule> &calculator);

private:
    Config config_;
};

}  // namespace fp

#endif  // FP_SOLVER_HPP
