#ifndef MS_CONSTRAINTS_ROOM_CAPACITY_COST_HPP
#define MS_CONSTRAINTS_ROOM_CAPACITY_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Hard, weight 1: roomCapacityCost — a room too small for the meeting placed
// in it. Raw cost per meeting = max(0, required + preferred attendees - room
// capacity), in seats; 0 = every meeting fits its room.
struct RoomCapacityCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RoomCapacityCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("roomCapacityCost", weight, kDefaultLevel) {}
    double assignmentCost(const MeetingSchedule& schedule, int unit,
                          int assignmentIndex) const override {
        const int meeting = schedule.meetingAssignments[assignmentIndex].meeting;
        const int shortage = schedule.requiredCapacityOf[meeting] -
                             schedule.rooms[schedule.roomIndexOf(unit)].capacity;
        return shortage > 0 ? static_cast<double>(shortage) : 0.0;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_ROOM_CAPACITY_COST_HPP
