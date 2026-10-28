#ifndef MS_CONSTRAINTS_ASSIGN_LARGER_ROOMS_FIRST_HPP
#define MS_CONSTRAINTS_ASSIGN_LARGER_ROOMS_FIRST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft (default, weight 1): assignLargerRoomsFirst — for every room strictly
// larger than the one a meeting got, the capacity difference. It fills the big
// rooms first, so the small ones stay free for the meetings that fit only
// there. The per-room total is precomputed by ScoreCalculator::prepare.
struct AssignLargerRoomsFirst final : PerAssignmentConstraint
{
    explicit AssignLargerRoomsFirst(double weight = 1.0)
        : PerAssignmentConstraint("assignLargerRoomsFirst",
                                  weight,
                                  /*hard=*/false)
    {
    }
    double assignmentCost(const MeetingSchedule &schedule,
                          int unit,
                          int /*assignmentIndex*/) const override
    {
        return static_cast<double>(
            schedule.largerRoomPenalty[schedule.roomIndexOf(unit)]);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_ASSIGN_LARGER_ROOMS_FIRST_HPP
