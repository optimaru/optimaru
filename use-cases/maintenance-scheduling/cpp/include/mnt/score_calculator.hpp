#ifndef MNT_SCORE_CALCULATOR_HPP
#define MNT_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "mnt/domain.hpp"

namespace mnt
{

// Port of the Timefold maintenance-scheduling
// MaintenanceScheduleConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore; day counts are CALENDAR days, exactly Java's
// ChronoUnit.DAYS.between on the [startDate, endDate) intervals):
//   hard 1. crewConflict        -1 per overlapping day per same-crew job pair
//   hard 2. minStartDate        -1 per day a job starts before minStartDate
//   hard 3. maxEndDate          -1 per day a job ends after maxEndDate
//   soft 4. beforeIdealEndDate  -1 per day a job ends before idealEndDate
//   soft 5. afterIdealEndDate   -1'000'000 per day a job ends after
//                               idealEndDate
//   soft 6. tagConflict         -1'000 per overlapping day per shared tag
//                               per overlapping job pair
//
// The two pairwise constraints reduce to per-day occupancy pair counts:
// summed over all days d, C(jobs of crew c covering d, 2) equals the summed
// pairwise overlap of same-crew jobs because job intervals are contiguous —
// the same identity for (tag, day). Unassigned jobs (crewDate == -1)
// contribute nothing: the construction heuristic scores partially
// initialized solutions (matching Java's forEach, which skips entities with
// a null planning variable).
class ScoreCalculator
{
public:
    static Score calculateScore(MaintenanceSchedule &schedule);

    // Interns tags into dense ids, builds the start-date value range
    // (business days of the calendar), the endDate shadow table and the day
    // horizon. Idempotent (guarded by MaintenanceSchedule::prepared). Must
    // run before scoring; calculateScore/solve call it.
    static void prepare(MaintenanceSchedule &schedule);

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }
};

}  // namespace mnt

#endif  // MNT_SCORE_CALCULATOR_HPP
