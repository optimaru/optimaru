#ifndef FAB_CONSTRAINTS_MACHINE_UNAVAILABILITY_COST_HPP
#define FAB_CONSTRAINTS_MACHINE_UNAVAILABILITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Hard, weight 1: machineUnavailabilityCost — processing time inside a
// machine's unavailable windows (preventive maintenance, downtime). Raw cost
// per machine = the sum over its assignments and its windows of the overlap
// time. 0 = nothing runs while its machine is down.
struct MachineUnavailabilityCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MachineUnavailabilityCost;

    explicit MachineUnavailabilityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("machineUnavailabilityCost", weight, kDefaultLevel, kTerm) {}

    static std::int64_t machineCost(const SemiconductorSchedule& schedule, int machine,
                                    const PlacementList& rows, std::vector<std::int64_t>* items) {
        const Machine& m = schedule.machines[static_cast<std::size_t>(machine)];
        std::int64_t total = 0;
        for (const ScorePlacement* p : rows) {
            for (const TimeWindow& window : m.unavailable)
                total = checked::add(
                        total, terms::emit(items, terms::overlap(p->start, p->end, window.start,
                                                                 window.end)));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MACHINE_UNAVAILABILITY_COST_HPP
