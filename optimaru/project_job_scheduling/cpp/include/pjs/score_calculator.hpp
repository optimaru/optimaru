#ifndef PJS_SCORE_CALCULATOR_HPP
#define PJS_SCORE_CALCULATOR_HPP

#include "pjs/domain.hpp"

namespace pjs {

// The project-job-scheduling constraints as one monolithic calculator.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (hard / medium / soft, all penalties), at the components'
// kDefaultWeight (1 each; the medium weight is in medium units):
//   hard 1. nonRenewableResourceCapacityCost  per non-renewable resource
//                                             whose total requirement over
//                                             all assigned execution modes
//                                             exceeds its capacity:
//                                             -(total - capacity)
//   hard 2. renewableResourceCapacityCost     per (renewable resource, day)
//                                             whose total requirement over
//                                             the allocations busy that day
//                                             exceeds its capacity:
//                                             -(total - capacity)
//   medium 3. projectDelayCost                per SINK allocation ending
//                                             after its project's
//                                             criticalPathEndDate
//                                             (= releaseDate +
//                                             criticalPathDuration):
//                                             -(endDate - criticalPathEndDate)
//   soft 4. makespanCost                      -(max SINK endDate) — one
//                                             penalty for the whole schedule
//
// The dates are derived, computed in topological order:
//   predecessorsDoneDate = max(0, max over predecessors of endDate)
//   startDate            = predecessorsDoneDate + delay      (unassigned: 0)
//   endDate              = startDate + executionMode.duration (unassigned: 0)
// An allocation is busy on the days [startDate, endDate). Unassigned
// allocations (modeDelay == -1) pass dates through with delay 0 / duration 0
// and contribute no resource usage, so the construction heuristic scores
// partially initialized solutions consistently.
class ScoreCalculator {
public:
    // Full recompute; also refreshes every allocation's derived dates and
    // stores schedule.score.
    static Score score(ProjectJobSchedule& schedule);

    // The same recompute in the engine's score type (see toEngineScore).
    static optimaru::Score calculateEngineScore(ProjectJobSchedule& schedule) {
        return toEngineScore(score(schedule));
    }

    // Validates the instance (indices in range, every job has at least one
    // execution mode, precedence graph acyclic — throws
    // std::invalid_argument otherwise), computes maxModeCount, the successor
    // lists and a topological order. Idempotent (guarded by
    // ProjectJobSchedule::prepared). Must run before scoring; score
    // and Solver::solve call it.
    static void prepare(ProjectJobSchedule& schedule);

    // Recomputes predecessorsDoneDate/startDate/endDate for every allocation
    // in topological order. Requires prepare().
    static void updateDates(ProjectJobSchedule& schedule);
};

}  // namespace pjs

#endif  // PJS_SCORE_CALCULATOR_HPP
