#ifndef MS_CONSTRAINTS_ROOM_CHANGE_COST_HPP
#define MS_CONSTRAINTS_ROOM_CHANGE_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft, weight 1: roomChangeCost — a shared attendee whose next meeting
// starts within two grains but in a different room, so they have to pack up
// and walk. Unlike the three conflict rules this one is not about overlap: it
// fires precisely when the meetings do NOT overlap much. Raw cost (global) =
// for every such pair of meetings, the shared-attendance multiplicity
// (attendee room switches); 0 = nobody changes rooms between close meetings.
//
// "Next" is the Java TimeGrain comparator (day, then starting minute) and
// must be strict — two meetings starting at the same grain have no earlier
// one, so nobody walks between them.
struct RoomChangeCost final : AttendanceArcConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit RoomChangeCost(double weight = kDefaultWeight)
        : AttendanceArcConstraint("roomChangeCost", weight, kDefaultLevel)
    {
    }

protected:
    std::int64_t marginal(const MeetingSchedule &schedule,
                          int assignmentIndex,
                          int unit) const override
    {
        const int meeting
            = schedule.meetingAssignments[assignmentIndex].meeting;
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        const int position = schedule.timeGrainPosOf(unit);
        std::int64_t total = 0;
        for (int arcIndex : arcsOf(meeting))
        {
            const ConflictArc &arc = schedule.conflictArcs[arcIndex];
            if (arc.attendanceWeight == 0)
                continue;
            int partnerAssignment = -1;
            int partnerUnit = -1;
            if (!partnerOf(
                    schedule, arc, meeting, partnerAssignment, partnerUnit))
                continue;
            if (schedule.roomIndexOf(unit) == schedule.roomIndexOf(partnerUnit))
                continue;  // same room: nobody has to move
            const int partnerPosition = schedule.timeGrainPosOf(partnerUnit);
            const int order = ScoreCalculator::compareTimeGrains(
                schedule.timeGrains[position],
                schedule.timeGrains[partnerPosition]);
            if (order == 0)
                continue;
            int partnerStart = 0;
            int partnerEnd = 0;
            detail::spanOf(schedule,
                           partnerUnit,
                           partnerAssignment,
                           partnerStart,
                           partnerEnd);
            const int firstEnd = order < 0 ? end : partnerEnd;
            const int secondStart = order < 0 ? partnerStart : start;
            if (secondStart - firstEnd <= 2)
                total += arc.attendanceWeight;
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_ROOM_CHANGE_COST_HPP
