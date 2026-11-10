#ifndef FP_SCORE_CALCULATOR_HPP
#define FP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "fp/domain.hpp"

namespace fp
{

// Per-constraint raw penalty magnitudes of one full recompute (all >= 0; the
// score negates them), for the C++ tests. Each member is named after its
// constraint's registry name (fp/constraints/<subject>_cost.hpp) and holds
// that constraint's raw cost summed over its units, the same total
// constraint_costs() reports per name. toScore() applies each component's
// kDefaultWeight (score_calculator.cpp). Not bound to Python:
// constraint_costs() is the one per-constraint surface there.
struct ConstraintBreakdown
{
    std::int64_t maxEndTimeCost = 0;                // hard, minutes late
    std::int64_t operatorCleaningConflictCost = 0;  // hard, overlap minutes
    std::int64_t afterIdealEndTimeCost = 0;         // medium, minutes late
    std::int64_t unassignedJobsCost = 0;  // medium, unassigned minutes
    std::int64_t squaredMakespanCost = 0;  // soft, sum of makespan^2

    // The hard/medium/soft score at the components' kDefaultWeight (all
    // integral, so the levels are exact int64).
    Score toScore() const;
};

// The food-packaging constraints as a straightforward full recompute over
// the job lists — the reference the incremental calculator is verified
// against.
//
// Constraints (hard / medium / soft, all penalize), the default assembly of
// fp/constraints.hpp at the components' kDefaultWeight (all 1):
//   hard   1. maxEndTimeCost               assigned job ending after
//                                          maxEndTime: -minutes(end -
//                                          maxEndTime)
//   hard   2. operatorCleaningConflictCost unique pair of assigned jobs whose
//                                          lines share an operator and whose
//                                          cleaning windows
//                                          [startCleaning, startProduction)
//                                          overlap: -overlap minutes. (Jobs
//                                          on the SAME line share its
//                                          operator and are included, but
//                                          their windows are sequential and
//                                          can never overlap.)
//   medium 3. afterIdealEndTimeCost        assigned job ending after
//                                          idealEndTime: -minutes(end -
//                                          ideal)
//   medium 4. unassignedJobsCost           unassigned job: -duration minutes
//   soft   5. squaredMakespanCost          last job of each line:
//                                          -minutes(end - line start)^2
// minStartTime and priority are data fields that no constraint reads.
class ScoreCalculator
{
public:
    // Builds the engine view (pinning translation) and validates the
    // instance. Throws std::invalid_argument on: a job in two lists, a
    // pinned job after an unpinned one on the same line, negative durations,
    // a bad product index, or a wrong-sized cleanup matrix. Pinned jobs in
    // no list are moved into the unassigned pool (they must stay
    // unassigned). Idempotent per solution.
    static void prepare(PackagingSchedule &schedule);

    // Recomputes every job's derived fields (lineIndex,
    // startCleaningDateTime, startProductionDateTime, endDateTime) from the
    // current lists. Throws std::invalid_argument when a needed cleanup
    // entry is missing.
    static void updateShadowVariables(PackagingSchedule &schedule);

    // updateShadowVariables + the five constraints; stores the score in
    // schedule.score and returns it.
    static Score score(PackagingSchedule &schedule);

    // Same recompute, split per constraint (raw magnitudes; see toScore()).
    // C++ only (tests); Python reads constraint_costs() instead.
    static ConstraintBreakdown analyze(PackagingSchedule &schedule);
};

}  // namespace fp

#endif  // FP_SCORE_CALCULATOR_HPP
