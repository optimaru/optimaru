#ifndef ES_SOLVER_HPP
#define ES_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "es/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace es {

// Thin domain-side wrapper around the generic engine. It wires up the domain
// pieces (the incremental score calculator and the guided move-selection
// heuristics) and delegates solving to
// optimaru::Solver<EmployeeSchedule>.
class Solver {
public:
    struct Config {
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires (termination is by spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        std::size_t lateAcceptanceSize = 400;
        std::uint64_t randomSeed = 0;

        // Fraction of the total time budget reserved for the post-local-search
        // greedy hard-repair. Local search runs for spentLimit*(1-fraction) and
        // the repair gets the rest, so total wall time still respects
        // spentLimit. Clamped to [0, 0.9).
        //
        // Default 0 (off): local search already returns its best-so-far — the
        // least-infeasible solution it found. The extra greedy pass was measured
        // not to improve on that best (a single relocation cannot dent
        // violations spread over a worker's whole run) and its reserve only
        // costs local-search time. Kept as a knob for instances where local
        // search ends a few points short of feasible and a final greedy push
        // closes the gap.
        double greedyRepairBudgetFraction = 0.0;

        // Independent parallel starts. Each worker receives randomSeed + i
        // and the full wall-clock budget; the best solution wins. This is
        // generally more scalable than synchronizing candidate evaluation
        // around one mutable incremental score director.
        // Only solve(schedule) honors this; solve(schedule, calculator) ignores
        // it and runs one search. Python goes through the calculator overload
        // and gets its parallel starts from separate processes instead (see
        // es_scheduling.models.solve_request).
        int parallelSolverCount = 1;

        // Guided move selection (A: pick a skill-capable, available target
        // employee; B: bias the shift toward employees with hard violations).
        // Set false for plain uniform-random move selection.
        bool guidedMoves = true;
        bool employeeGreedyConstruction = true;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the late-acceptance search can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick above (engine
        // RuinRecreateConfig::asMoveWeight). Probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move. It draws from the
        // same adaptive ruin-operator pool as the kick, so with
        // employeeRuinMoves on the domain's own employeeHotRuin is one of the
        // operators it can pick — the difference from the kick is the
        // cadence (every step by weight, not on stagnation) and that the
        // acceptor gets a veto.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Best-improvement descent over one nearby shift pair (engine
        // LocalSearchConfig::descentWeight): enumerate every employee for
        // both shifts plus their swap, apply the best improving one, repeat
        // until the pair is locally optimal. Always improving, so it bypasses
        // the acceptor. Picks its first shift through the same violation
        // guidance as the change moves when guidedMoves is on.
        //
        // One descent costs ~2 * employeeCount score evaluations, which is
        // what makes it affordable HERE: the value range is the employee
        // list, usually orders of magnitude smaller than a routing domain's
        // positions or a timetable's slot x room grid. A sweep picked 0.02:
        // a clear gain on large rosters, a wash on small ones. Past ~0.05 the
        // descent starts starving the ordinary move mix and the gain shrinks.
        double descentWeight = 0.02;
        int descentMaxSweeps = 4;

        // Temporal nearby selection (engine NearbySelectionConfig): distance
        // between two shifts is |start - start| in minutes, so swap partners,
        // descent pairs and the LNS related-ruin cluster stay inside the time
        // window where the pairwise hard constraints (overlap, one-per-day,
        // min-rest) actually interact. Neighbor-list size is auto-derived from
        // the densest 24h window of the instance. Same pattern as
        // school-timetabling's adopted same-group nearby.
        bool nearbyShifts = true;
        // Default 0.65: measured best compromise across roster sizes. 0.8 wins
        // bigger on rosters of ~1000 shifts but costs small rosters their soft
        // budget once hard converges (same-day swaps cannot move the date-based
        // soft terms). For large-only workloads prefer 0.8.
        double nearbyShiftsProbability = 0.65;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Employee-scheduling-specific BASIC operators registered through the
        // engine's custom move hooks. They complement the generic change/swap
        // moves with stronger domain moves while keeping the same score
        // calculator and acceptor protocol.
        bool smartChangeMoves = false;
        int smartChangeCandidateCount = 6;
        bool employeeDayRepairMoves = true;
        bool employeeRunRepairMoves = true;
        int employeeRunRepairMaxDays = 3;
        bool employeeWindowMoves = true;
        int employeeWindowMaxDays = 3;
        // Cap how often the adaptive custom-operator pool gets a turn. Window
        // swaps are much costlier than sampled change/swap moves and can
        // otherwise monopolize the selector after one reward. 0.30-0.40 is a
        // plateau in interleaved A/B runs across roster sizes. Re-measure if
        // move economics shift.
        double customMoveProbability = 0.30;
        bool employeeRuinMoves = true;

        // Allow the search to leave a shift UNASSIGNED (employeeIndex = -1).
        // The default fills every shift (the value range is the employee list,
        // so no move ever produces -1); enable this to let local search empty a
        // slot when that improves the score — needed when shifts are optional
        // coverage slots (staffing floor via shift_coverage) rather than fixed
        // demand. Default off: with the unassigned_shift hard rule (the usual
        // setup) an empty slot is penalized and refilled anyway.
        bool employeeUnassignMoves = false;

        // LIST move mix. Exposed for parity with the generic engine; employee
        // scheduling is BASIC, so these do not affect this domain.
        double relocateMoveWeight = 0.5;
        double listSwapMoveWeight = 0.3;
        double twoOptMoveWeight = 0.2;
        double orOptMoveWeight = 0.0;
        double tailSwapMoveWeight = 0.0;

        std::function<void(std::int64_t, const Score&)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves with the built-in monolithic incremental calculator (the fixed
    // default constraints, the hand-tuned fast path).
    void solve(EmployeeSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see es/constraints.hpp):
    // pick constraints and weights, then pass the assembly here. The same
    // guided move selection applies — violation guidance comes from the
    // calculator's penalizedUnits(). The final schedule.score is computed by
    // THIS calculator (custom weights included), and the calculator is left
    // primed on the final solution so constraintTotals() explains the score.
    // Structurally compatible delta constraints are grouped automatically;
    // unsupported constraints remain on the individual generic delta path.
    // Always one search: Config::parallelSolverCount is ignored here.
    void solve(EmployeeSchedule& schedule,
               optimaru::DecomposedScoreCalculator<EmployeeSchedule>& calculator);

private:
    Config config_;
};

}  // namespace es

#endif  // ES_SOLVER_HPP
