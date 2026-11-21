#ifndef MS_CONSTRAINTS_OVERTIME_COST_HPP
#define MS_CONSTRAINTS_OVERTIME_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Hard, weight 1: overtimeCost — a meeting that runs past the end of the
// calendar, i.e. no TimeGrain exists at its last grain index. Raw cost
// per meeting = that last grain index (NOT the overrun), so overrunning
// further costs more; 0 = every
// meeting ends on an existing grain.
struct OvertimeCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit OvertimeCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("overtimeCost", weight, kDefaultLevel) {}
    double assignmentCost(const MeetingSchedule& schedule, int unit,
                          int assignmentIndex) const override {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        const int lastIndex = end - 1;
        if (schedule.grainCountAtOffset(lastIndex - schedule.grainIndexBase) != 0) return 0.0;
        return static_cast<double>(lastIndex);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_OVERTIME_COST_HPP
