#ifndef MS_CONSTRAINTS_LARGER_ROOM_CAPACITY_COST_HPP
#define MS_CONSTRAINTS_LARGER_ROOM_CAPACITY_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Soft, weight 1: largerRoomCapacityCost — the capacity left unused in
// larger rooms. Raw cost per meeting = the sum, over every room strictly
// larger than the one it got, of the capacity difference (seats; NOT a
// capacity overflow); 0 = the meeting sits in a largest room. It fills the
// big rooms first, so the small ones stay free for the meetings that fit only
// there. The per-room total is precomputed by ScoreCalculator::prepare.
struct LargerRoomCapacityCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LargerRoomCapacityCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("largerRoomCapacityCost", weight, kDefaultLevel) {}
    double assignmentCost(const MeetingSchedule& schedule, int unit,
                          int /*assignmentIndex*/) const override {
        return static_cast<double>(schedule.largerRoomPenalty[schedule.roomIndexOf(unit)]);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_LARGER_ROOM_CAPACITY_COST_HPP
