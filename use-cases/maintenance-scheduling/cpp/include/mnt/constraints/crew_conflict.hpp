#ifndef MNT_CONSTRAINTS_CREW_CONFLICT_HPP
#define MNT_CONSTRAINTS_CREW_CONFLICT_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Hard (default, weight 1): crewConflict — one point per calendar day two
// jobs of the same crew overlap. One owner row per crew, so a job occupies
// exactly one row for the days it runs.
//
// Global because the pairs span units: two jobs of one crew that start on
// different dates sit in different (start date, crew) cells.
struct CrewConflict final : DayOverlapGlobalConstraint
{
    explicit CrewConflict(double weight = 1.0)
        : DayOverlapGlobalConstraint("crewConflict", weight, /*hard=*/true)
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

#endif  // MNT_CONSTRAINTS_CREW_CONFLICT_HPP
