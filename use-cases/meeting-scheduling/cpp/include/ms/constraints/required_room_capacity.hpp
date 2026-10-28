#ifndef MS_CONSTRAINTS_REQUIRED_ROOM_CAPACITY_HPP
#define MS_CONSTRAINTS_REQUIRED_ROOM_CAPACITY_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard (default, weight 1): requiredRoomCapacity — how many attendees a room
// is short of holding the meeting placed in it.
struct RequiredRoomCapacity final : PerAssignmentConstraint
{
    explicit RequiredRoomCapacity(double weight = 1.0)
        : PerAssignmentConstraint("requiredRoomCapacity", weight, /*hard=*/true)
    {
    }
    double assignmentCost(const MeetingSchedule &schedule,
                          int unit,
                          int assignmentIndex) const override
    {
        const int meeting
            = schedule.meetingAssignments[assignmentIndex].meeting;
        const int shortage
            = schedule.requiredCapacityOf[meeting]
              - schedule.rooms[schedule.roomIndexOf(unit)].capacity;
        return shortage > 0 ? static_cast<double>(shortage) : 0.0;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_REQUIRED_ROOM_CAPACITY_HPP
