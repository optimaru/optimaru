#ifndef FAB_CONSTRAINTS_BATCH_CAPACITY_COST_HPP
#define FAB_CONSTRAINTS_BATCH_CAPACITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: batchCapacityCost — capacity-time above the simultaneous
// lot limit of a batch tool. Raw cost per batch machine = the time-integrated
// excess of its assignments over its batch capacity (the overload area of
// MachineCapacityCost). 0 = no batch tool ever runs more lots than it holds.
struct BatchCapacityCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::BatchCapacityCost;

    explicit BatchCapacityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("batchCapacityCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t machineCost(const SemiconductorSchedule &schedule, int machine,
                                    const PlacementList &rows,
                                    std::vector<std::int64_t> *items)
    {
        const Machine &m = schedule.machines[static_cast<std::size_t>(machine)];
        if (!m.isBatch())
            return 0;
        return terms::emit(items, terms::overloadArea(rows, m.batchCapacity));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_BATCH_CAPACITY_COST_HPP
