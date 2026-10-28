#ifndef MNT_CONSTRAINTS_TAG_CONFLICT_HPP
#define MNT_CONSTRAINTS_TAG_CONFLICT_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Soft (default, weight 1'000): tagConflict — one point per calendar day two
// jobs that share a tag overlap, counted once per SHARED tag. One owner row
// per tag, so a job occupies as many rows as it carries tags.
//
// Global for the same reason as crewConflict: two jobs sharing a tag are
// generally in different (start date, crew) cells.
struct TagConflict final : DayOverlapGlobalConstraint
{
    explicit TagConflict(double weight = 1000.0)
        : DayOverlapGlobalConstraint("tagConflict", weight, /*hard=*/false)
    {
    }
    int ownerCount(const MaintenanceSchedule &schedule) const override
    {
        return schedule.tagCount;
    }
    void ownersOf(const MaintenanceSchedule &schedule,
                  int /*unit*/,
                  int jobIndex,
                  std::vector<int> &owners) const override
    {
        const Job &job = schedule.jobs[jobIndex];
        owners.insert(owners.end(), job.tagIds.begin(), job.tagIds.end());
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_TAG_CONFLICT_HPP
