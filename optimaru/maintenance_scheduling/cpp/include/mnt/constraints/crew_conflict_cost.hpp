#ifndef MNT_CONSTRAINTS_CREW_CONFLICT_COST_HPP
#define MNT_CONSTRAINTS_CREW_CONFLICT_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Hard, weight 1: crewConflictCost — the calendar days two jobs of the same
// crew overlap. Raw cost (one global total) = the summed pairwise overlap in
// days over every same-crew job pair, i.e. the sum over (crew, day) of
// C(jobs covering it, 2); 0 = no crew works two jobs on one day. One owner
// row per crew, so a job occupies exactly one row for the days it runs.
//
// Global because the pairs span units: two jobs of one crew that start on
// different dates sit in different (start date, crew) cells.
struct CrewConflictCost final : DayOverlapGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit CrewConflictCost(double weight = kDefaultWeight)
        : DayOverlapGlobalConstraint("crewConflictCost", weight, kDefaultLevel)
    {
    }
    int ownerCount(const MaintenanceSchedule &schedule) const override
    {
        return schedule.crewCount();
    }
    void ownersOf(const MaintenanceSchedule &schedule,
                  int unit,
                  int /*jobIndex*/,
                  std::vector<int> &owners) const override
    {
        owners.push_back(schedule.crewIndexOf(unit));
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_CREW_CONFLICT_COST_HPP
