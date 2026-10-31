#ifndef FAB_CONSTRAINTS_MACHINE_ELIGIBILITY_COST_HPP
#define FAB_CONSTRAINTS_MACHINE_ELIGIBILITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: machineEligibilityCost — operations that are not run on an
// eligible machine for their route step. Raw cost per operation row (lot
// order, route order) = 1 when it has no assignment, when no eligible option
// names the assigned machine, or when the assignment's length differs from
// that option's duration; else 0. With a machine listed twice among the
// options the LAST one is read (the 0.1.x lookup). 0 = every operation is
// assigned to an eligible machine for exactly its duration.
struct MachineEligibilityCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MachineEligibilityCost;

    explicit MachineEligibilityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("machineEligibilityCost", weight, kDefaultLevel, kTerm)
    {
    }

    // One row; `p` is its assignment (nullptr = none).
    static std::int64_t rowCost(const SemiconductorSchedule &schedule, int row,
                                const ScorePlacement *p)
    {
        if (p == nullptr)
            return 1;
        const MachineOption *option = schedule.operationOfRow(row).lastOptionOn(p->machine);
        return option == nullptr || p->end - p->start != option->duration ? 1 : 0;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MACHINE_ELIGIBILITY_COST_HPP
