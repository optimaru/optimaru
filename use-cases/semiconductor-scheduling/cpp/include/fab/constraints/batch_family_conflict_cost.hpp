#ifndef FAB_CONSTRAINTS_BATCH_FAMILY_CONFLICT_COST_HPP
#define FAB_CONSTRAINTS_BATCH_FAMILY_CONFLICT_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: batchFamilyConflictCost — overlap time of different recipe
// families on one batch tool. Raw cost per batch machine: in machine order
// (start, end, lot id), each assignment is compared with the later ones until
// one starts at or after its end; every pair of different families adds its
// overlap time. 0 = only one family runs at a time on every batch tool.
struct BatchFamilyConflictCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::BatchFamilyConflictCost;

    explicit BatchFamilyConflictCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("batchFamilyConflictCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t machineCost(const SemiconductorSchedule &schedule, int machine,
                                    const PlacementList &rows,
                                    std::vector<std::int64_t> *items)
    {
        if (!schedule.machines[static_cast<std::size_t>(machine)].isBatch())
            return 0;
        std::int64_t total = 0;
        for (std::size_t left = 0; left < rows.size(); ++left)
        {
            const ScorePlacement *a = rows[left];
            const int family = terms::familyOf(schedule, a);
            for (std::size_t right = left + 1; right < rows.size(); ++right)
            {
                const ScorePlacement *b = rows[right];
                if (b->start >= a->end)
                    break;
                if (terms::familyOf(schedule, b) != family)
                    total = checked::add(total, terms::emit(items, terms::overlap(a->start, a->end, b->start, b->end)));
            }
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_BATCH_FAMILY_CONFLICT_COST_HPP
