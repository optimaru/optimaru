#ifndef FAB_CONSTRAINTS_MACHINE_CAPACITY_COST_HPP
#define FAB_CONSTRAINTS_MACHINE_CAPACITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Hard, weight 1: machineCapacityCost — processing time that overlaps on a
// unary tool (batch capacity 1). Raw cost per unary machine = the
// time-integrated excess of its assignments over capacity 1 (sweep +1 at
// each start, -1 at each end; sum max(0, active - 1) * gap). 0 = no two
// assignments overlap on any unary tool.
struct MachineCapacityCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MachineCapacityCost;

    explicit MachineCapacityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("machineCapacityCost", weight, kDefaultLevel, kTerm) {}

    // One machine's assignments, in machine order; batch tools contribute
    // no item (BatchCapacityCost covers them).
    static std::int64_t machineCost(const SemiconductorSchedule& schedule, int machine,
                                    const PlacementList& rows, std::vector<std::int64_t>* items) {
        if (schedule.machines[static_cast<std::size_t>(machine)].isBatch()) return 0;
        return terms::emit(items, terms::overloadArea(rows, 1));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MACHINE_CAPACITY_COST_HPP
