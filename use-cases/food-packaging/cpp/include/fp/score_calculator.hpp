#ifndef FP_SCORE_CALCULATOR_HPP
#define FP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "fp/domain.hpp"

namespace fp
{

// Per-constraint penalty magnitudes (all >= 0; the score negates them), for
// tests and the Python bindings — the stand-in for Timefold's
// SolutionManager.analyze().
struct ConstraintBreakdown
{
    std::int64_t maxEndDateTime = 0;            // hard, minutes late
    std::int64_t operatorCleaningConflict = 0;  // hard, overlap minutes
    std::int64_t idealEndDateTime = 0;          // medium, minutes late
    std::int64_t maximizeJobsAssigned = 0;      // medium, unassigned minutes
    std::int64_t minimizeMakespan = 0;          // soft, sum of makespan^2

    Score toScore() const
    {
        return {-(maxEndDateTime + operatorCleaningConflict),
                -(idealEndDateTime + maximizeJobsAssigned),
                -minimizeMakespan};
    }
};

// Port of org.acme.foodpackaging.solver.FoodPackagingConstraintProvider as a
// straightforward full recompute over the job lists — the reference the
// incremental calculator is verified against.
//
// Constraints (HardMediumSoftScore, all penalize):
//   hard   1. maxEndDateTime           assigned job ending after maxEndTime:
//                                      -minutes(end - maxEndTime)
//   hard   2. operatorCleaningConflict unique pair of assigned jobs whose
//                                      lines share an operator and whose
//                                      cleaning windows
//                                      [startCleaning, startProduction)
//                                      overlap: -overlap minutes. (Jobs on
//                                      the SAME line share its operator and
//                                      are included, but their windows are
//                                      sequential and can never overlap.)
//   medium 3. idealEndDateTime         assigned job ending after
//                                      idealEndTime: -minutes(end - ideal)
//   medium 4. maximizeJobsAssigned     unassigned job: -duration minutes
//   soft   5. minimizeMakespan         last job of each line: -minutes(end -
//                                      line start)^2
// minStartTime and priority exist in the Java domain but are read by no
// constraint (kept as data for fidelity).
class ScoreCalculator
{
public:
    // Builds the engine view (pinning translation) and validates the
    // instance. Throws std::invalid_argument on: a job in two lists, a
    // pinned job after an unpinned one on the same line, negative durations,
    // a bad product index, or a wrong-sized cleanup matrix. Pinned jobs in
    // no list are moved into the unassigned pool (they must stay unassigned,
    // like Java's pinned Job with line == null). Idempotent per solution.
    static void prepare(PackagingSchedule &schedule);

    // Recomputes every job's shadow variables (lineIndex,
    // startCleaningDateTime, startProductionDateTime, endDateTime) from the
    // current lists — the Java shadow chain. Throws std::invalid_argument
    // when a needed cleanup entry is missing (Java's
    // Product.getCleanupDuration IllegalArgumentException).
    static void updateShadowVariables(PackagingSchedule &schedule);

    // updateShadowVariables + the five constraints; stores the score in
    // schedule.score and returns it.
    static Score calculateScore(PackagingSchedule &schedule);

    // Same recompute, split per constraint (magnitudes; see toScore()).
    static ConstraintBreakdown analyze(PackagingSchedule &schedule);
};

}  // namespace fp

#endif  // FP_SCORE_CALCULATOR_HPP
