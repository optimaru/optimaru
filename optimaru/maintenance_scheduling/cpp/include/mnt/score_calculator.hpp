#ifndef MNT_SCORE_CALCULATOR_HPP
#define MNT_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "mnt/domain.hpp"

namespace mnt
{

// The six maintenance-scheduling constraints.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (day counts are CALENDAR days
// on the [startDate, endDate) intervals), the default
// assembly of mnt/constraints.hpp at the components' kDefaultWeight:
//   hard 1. crewConflictCost        -1 per overlapping day per same-crew job
//                                   pair
//   hard 2. minStartDateCost        -1 per day a job starts before
//                                   minStartDate
//   hard 3. maxEndDateCost          -1 per day a job ends after maxEndDate
//   soft 4. beforeIdealEndDateCost  -1 per day a job ends before
//                                   idealEndDate
//   soft 5. afterIdealEndDateCost   -1'000'000 per day a job ends after
//                                   idealEndDate
//   soft 6. tagConflictCost         -1'000 per overlapping day per shared tag
//                                   per overlapping job pair
//
// The two pairwise constraints reduce to per-day occupancy pair counts:
// summed over all days d, C(jobs of crew c covering d, 2) equals the summed
// pairwise overlap of same-crew jobs because job intervals are contiguous —
// the same identity for (tag, day). Unassigned jobs (crewDate == -1)
// contribute nothing: the construction heuristic scores partially
// initialized solutions.
class ScoreCalculator
{
public:
    static Score score(MaintenanceSchedule &schedule);

    // Interns tags into dense ids, builds the start-date value range
    // (business days of the calendar), the endDate table and the day
    // horizon. Idempotent (guarded by MaintenanceSchedule::prepared). Must
    // run before scoring; score/solve call it.
    static void prepare(MaintenanceSchedule &schedule);

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }
};

}  // namespace mnt

#endif  // MNT_SCORE_CALCULATOR_HPP
