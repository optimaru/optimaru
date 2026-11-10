#ifndef MNT_CONSTRAINTS_TAG_CONFLICT_COST_HPP
#define MNT_CONSTRAINTS_TAG_CONFLICT_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Soft, weight 1'000: tagConflictCost — the calendar days two jobs that
// share a tag overlap, counted once per SHARED tag. Raw cost (one global
// total) = the sum over (tag, day) of C(jobs covering it, 2), i.e. a pair
// sharing k tags and overlapping d days counts k * d; 0 = no two jobs with a
// common tag run on the same day. One owner row per tag, so a job occupies
// as many rows as it carries tags.
//
// Global for the same reason as crewConflictCost: two jobs sharing a tag are
// generally in different (start date, crew) cells.
struct TagConflictCost final : DayOverlapGlobalConstraint
{
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TagConflictCost(double weight = kDefaultWeight)
        : DayOverlapGlobalConstraint("tagConflictCost", weight, kDefaultLevel)
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

#endif  // MNT_CONSTRAINTS_TAG_CONFLICT_COST_HPP
