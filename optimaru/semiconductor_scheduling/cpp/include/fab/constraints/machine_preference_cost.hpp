#ifndef FAB_CONSTRAINTS_MACHINE_PREFERENCE_COST_HPP
#define FAB_CONSTRAINTS_MACHINE_PREFERENCE_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Soft, weight 1: machinePreferenceCost — qualification, yield or operating
// preference cost of the chosen equipment. Raw cost per operation row = the
// preference cost of the LAST eligible option on its assigned machine
// (charged even when the duration does not match), 0 when it has no
// assignment or the machine is not eligible. 0 = only zero-cost tools are
// used.
struct MachinePreferenceCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::MachinePreferenceCost;

    explicit MachinePreferenceCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("machinePreferenceCost", weight, kDefaultLevel, kTerm) {}

    static std::int64_t rowCost(const SemiconductorSchedule& schedule, int row,
                                const ScorePlacement* p) {
        if (p == nullptr) return 0;
        const MachineOption* option = schedule.operationOfRow(row).lastOptionOn(p->machine);
        return option == nullptr ? 0 : option->preferenceCost;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MACHINE_PREFERENCE_COST_HPP
