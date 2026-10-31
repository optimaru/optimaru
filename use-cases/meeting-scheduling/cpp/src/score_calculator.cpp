#include "ms/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ms
{

namespace
{

std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

// Attendance multiplicity of one person in one meeting (normally (1, 0) or
// (0, 1); Java allows duplicates through the setters, so counted generally).
struct PersonMeetingEntry
{
    int meeting = 0;
    int required = 0;
    int preferred = 0;
};

}  // namespace

void ScoreCalculator::prepare(MeetingSchedule &schedule)
{
    if (schedule.prepared)
        return;

    const int personCount = static_cast<int>(schedule.people.size());
    const int meetingCount = static_cast<int>(schedule.meetings.size());

    // --- validation -------------------------------------------------------
    for (const Meeting &meeting : schedule.meetings)
    {
        if (meeting.durationInGrains < 1)
        {
            throw std::invalid_argument("meeting " + meeting.id
                                        + ": durationInGrains must be >= 1");
        }
        for (int person : meeting.requiredAttendees)
        {
            if (person < 0 || person >= personCount)
                throw std::invalid_argument(
                    "meeting " + meeting.id
                    + ": required attendee index out of range");
        }
        for (int person : meeting.preferredAttendees)
        {
            if (person < 0 || person >= personCount)
                throw std::invalid_argument(
                    "meeting " + meeting.id
                    + ": preferred attendee index out of range");
        }
    }
    schedule.assignmentOfMeeting.assign(meetingCount, -1);
    for (int i = 0; i < static_cast<int>(schedule.meetingAssignments.size());
         ++i)
    {
        const MeetingAssignment &assignment = schedule.meetingAssignments[i];
        if (assignment.meeting < 0 || assignment.meeting >= meetingCount)
        {
            throw std::invalid_argument("assignment " + assignment.id
                                        + ": meeting index out of range");
        }
        if (schedule.assignmentOfMeeting[assignment.meeting] >= 0)
        {
            throw std::invalid_argument(
                "meeting " + schedule.meetings[assignment.meeting].id
                + " has more than one MeetingAssignment (the model is 1:1)");
        }
        schedule.assignmentOfMeeting[assignment.meeting] = i;
    }

    // --- grain occupancy span ---------------------------------------------
    schedule.maxDurationInGrains = 1;
    for (const Meeting &meeting : schedule.meetings)
    {
        schedule.maxDurationInGrains
            = std::max(schedule.maxDurationInGrains, meeting.durationInGrains);
    }
    int minIndex = 0;
    int maxIndex = -1;
    if (!schedule.timeGrains.empty())
    {
        minIndex = schedule.timeGrains.front().grainIndex;
        maxIndex = schedule.timeGrains.front().grainIndex;
        for (const TimeGrain &grain : schedule.timeGrains)
        {
            minIndex = std::min(minIndex, grain.grainIndex);
            maxIndex = std::max(maxIndex, grain.grainIndex);
        }
    }
    schedule.grainIndexBase = minIndex;
    schedule.occupancySpan
        = (maxIndex - minIndex + 1) + schedule.maxDurationInGrains;
    schedule.grainDaysAtOffset.assign(schedule.occupancySpan, {});
    for (const TimeGrain &grain : schedule.timeGrains)
    {
        schedule.grainDaysAtOffset[grain.grainIndex - minIndex].push_back(
            grain.dayOfYear);
    }

    // --- "assign larger rooms first" penalty per room ----------------------
    const int roomCount = schedule.roomCount();
    schedule.largerRoomPenalty.assign(roomCount, 0);
    for (int r = 0; r < roomCount; ++r)
    {
        for (int other = 0; other < roomCount; ++other)
        {
            const int diff
                = schedule.rooms[other].capacity - schedule.rooms[r].capacity;
            if (diff > 0)
                schedule.largerRoomPenalty[r] += diff;
        }
    }

    schedule.requiredCapacityOf.assign(meetingCount, 0);
    for (int m = 0; m < meetingCount; ++m)
    {
        schedule.requiredCapacityOf[m]
            = schedule.meetings[m].requiredCapacity();
    }

    // --- attendance conflict arcs ------------------------------------------
    // Per person, the meetings they attend with multiplicities; then per
    // meeting pair, the pairwise attendance weights (Java pairs attendance
    // facts, so multiplicities multiply).
    std::vector<std::vector<PersonMeetingEntry>> byPerson(personCount);
    for (int m = 0; m < meetingCount; ++m)
    {
        const Meeting &meeting = schedule.meetings[m];
        for (int person : meeting.requiredAttendees)
        {
            auto &entries = byPerson[person];
            if (entries.empty() || entries.back().meeting != m)
                entries.push_back({m, 0, 0});
            ++entries.back().required;
        }
        for (int person : meeting.preferredAttendees)
        {
            auto &entries = byPerson[person];
            if (entries.empty() || entries.back().meeting != m)
                entries.push_back({m, 0, 0});
            ++entries.back().preferred;
        }
    }
    schedule.selfRequiredPairs.assign(meetingCount, 0);
    schedule.selfPreferredPairs.assign(meetingCount, 0);
    schedule.selfRequiredPreferred.assign(meetingCount, 0);
    std::map<std::pair<int, int>, ConflictArc> arcByPair;
    for (const auto &entries : byPerson)
    {
        for (std::size_t a = 0; a < entries.size(); ++a)
        {
            const PersonMeetingEntry &e1 = entries[a];
            schedule.selfRequiredPairs[e1.meeting] += pairs(e1.required);
            schedule.selfPreferredPairs[e1.meeting] += pairs(e1.preferred);
            schedule.selfRequiredPreferred[e1.meeting]
                += static_cast<std::int64_t>(e1.required) * e1.preferred;
            for (std::size_t b = a + 1; b < entries.size(); ++b)
            {
                const PersonMeetingEntry &e2 = entries[b];
                const PersonMeetingEntry &lo
                    = e1.meeting < e2.meeting ? e1 : e2;
                const PersonMeetingEntry &hi
                    = e1.meeting < e2.meeting ? e2 : e1;
                ConflictArc &arc = arcByPair[{lo.meeting, hi.meeting}];
                arc.meeting1 = lo.meeting;
                arc.meeting2 = hi.meeting;
                arc.requiredWeight
                    += static_cast<std::int64_t>(lo.required) * hi.required;
                arc.preferredWeight
                    += static_cast<std::int64_t>(lo.preferred) * hi.preferred;
                arc.reqPrefForward
                    += static_cast<std::int64_t>(lo.required) * hi.preferred;
                arc.reqPrefReverse
                    += static_cast<std::int64_t>(hi.required) * lo.preferred;
                arc.attendanceWeight
                    += static_cast<std::int64_t>(lo.required + lo.preferred)
                       * (hi.required + hi.preferred);
            }
        }
    }
    schedule.conflictArcs.clear();
    schedule.conflictArcs.reserve(arcByPair.size());
    schedule.stabilityNeighbors.assign(meetingCount, {});
    for (const auto &[key, arc] : arcByPair)
    {
        schedule.conflictArcs.push_back(arc);
        schedule.stabilityNeighbors[arc.meeting1].push_back(
            {arc.meeting2, arc.attendanceWeight});
        schedule.stabilityNeighbors[arc.meeting2].push_back(
            {arc.meeting1, arc.attendanceWeight});
    }

    schedule.prepared = true;
}

Breakdown ScoreCalculator::computeBreakdown(MeetingSchedule &schedule)
{
    prepare(schedule);
    Breakdown b;
    const int assignmentCount
        = static_cast<int>(schedule.meetingAssignments.size());

    // --- unary constraints (2, 4, 5, 8, 11 + same-meeting attendance) ------
    for (const MeetingAssignment &assignment : schedule.meetingAssignments)
    {
        if (assignment.grainRoom < 0)
            continue;  // unassigned: contributes nothing
        const int pos = schedule.timeGrainPosOf(assignment.grainRoom);
        const int room = schedule.roomIndexOf(assignment.grainRoom);
        const Meeting &meeting = schedule.meetings[assignment.meeting];
        const int startIndex = schedule.timeGrains[pos].grainIndex;
        const int lastIndex = startIndex + meeting.durationInGrains - 1;
        const int lastOffset = lastIndex - schedule.grainIndexBase;

        // 2. avoidOvertime: no TimeGrain fact exists at the last index.
        if (schedule.grainCountAtOffset(lastOffset) == 0)
            b.overtimeSum += lastIndex;
        // 4. requiredRoomCapacity.
        const int shortage = schedule.requiredCapacityOf[assignment.meeting]
                             - schedule.rooms[room].capacity;
        if (shortage > 0)
            b.roomCapacityShortage += shortage;
        // 5. startAndEndOnSameDay: -1 per grain fact at the last index on a
        // different day than the starting grain.
        b.startEndDayViolations
            += schedule.grainCountAtOffset(lastOffset)
               - schedule.grainCountAtOffsetOnDay(
                   lastOffset, schedule.timeGrains[pos].dayOfYear);
        // 8. doMeetingsAsSoonAsPossible.
        b.lastGrainIndexSum += lastIndex;
        // 11. assignLargerRoomsFirst.
        b.largerRoomCapacitySum += schedule.largerRoomPenalty[room];
        // Same-meeting attendance pairs (zero in sane data): the meeting
        // fully overlaps itself.
        b.requiredConflictGrains
            += schedule.selfRequiredPairs[assignment.meeting]
               * meeting.durationInGrains;
        b.preferredConflictGrains
            += schedule.selfPreferredPairs[assignment.meeting]
               * meeting.durationInGrains;
        b.requiredPreferredConflictGrains
            += schedule.selfRequiredPreferred[assignment.meeting]
               * meeting.durationInGrains;
    }

    // --- assignment pairs (1, 9, 10) ---------------------------------------
    for (int i = 0; i < assignmentCount; ++i)
    {
        const MeetingAssignment &left = schedule.meetingAssignments[i];
        if (left.grainRoom < 0)
            continue;
        const int startI
            = schedule.timeGrains[schedule.timeGrainPosOf(left.grainRoom)]
                  .grainIndex;
        const int durationI = schedule.meetings[left.meeting].durationInGrains;
        const int roomI = schedule.roomIndexOf(left.grainRoom);
        for (int j = i + 1; j < assignmentCount; ++j)
        {
            const MeetingAssignment &right = schedule.meetingAssignments[j];
            if (right.grainRoom < 0)
                continue;
            const int startJ
                = schedule.timeGrains[schedule.timeGrainPosOf(right.grainRoom)]
                      .grainIndex;
            const int durationJ
                = schedule.meetings[right.meeting].durationInGrains;
            const std::int64_t overlap = overlapGrains(
                startI, startI + durationI, startJ, startJ + durationJ);
            if (overlap > 0)
            {
                // 10. overlappingMeetings (distinct meetings: the model is
                // 1:1, so distinct assignments imply distinct meetings).
                b.overlappingMeetingGrains += overlap;
                // 1. roomConflict.
                if (roomI == schedule.roomIndexOf(right.grainRoom))
                    b.roomConflictGrains += overlap;
            }
            // 9. oneBreakBetweenConsecutiveMeetings (both directions; at
            // most one can match).
            if (startI + durationI == startJ)
                ++b.consecutiveMeetingPairs;
            if (startJ + durationJ == startI)
                ++b.consecutiveMeetingPairs;
        }
    }

    // --- attendance-weighted meeting pairs (3, 6, 7, 12) --------------------
    for (const ConflictArc &arc : schedule.conflictArcs)
    {
        const int i = schedule.assignmentOfMeeting[arc.meeting1];
        const int j = schedule.assignmentOfMeeting[arc.meeting2];
        if (i < 0 || j < 0)
            continue;
        const MeetingAssignment &left = schedule.meetingAssignments[i];
        const MeetingAssignment &right = schedule.meetingAssignments[j];
        if (left.grainRoom < 0 || right.grainRoom < 0)
            continue;
        const int posI = schedule.timeGrainPosOf(left.grainRoom);
        const int posJ = schedule.timeGrainPosOf(right.grainRoom);
        const int startI = schedule.timeGrains[posI].grainIndex;
        const int startJ = schedule.timeGrains[posJ].grainIndex;
        const int durationI = schedule.meetings[arc.meeting1].durationInGrains;
        const int durationJ = schedule.meetings[arc.meeting2].durationInGrains;
        const std::int64_t overlap = overlapGrains(
            startI, startI + durationI, startJ, startJ + durationJ);
        if (overlap > 0)
        {
            // 3. requiredAttendanceConflict / 7. preferredAttendanceConflict
            // / 6. requiredAndPreferredAttendanceConflict (both directions).
            b.requiredConflictGrains += arc.requiredWeight * overlap;
            b.preferredConflictGrains += arc.preferredWeight * overlap;
            b.requiredPreferredConflictGrains
                += (arc.reqPrefForward + arc.reqPrefReverse) * overlap;
        }
        // 12. roomStability: same attendee, different rooms, the earlier
        // meeting (by the TimeGrain day/minute comparator, strict) followed
        // within <= 2 grains.
        if (schedule.roomIndexOf(left.grainRoom)
            != schedule.roomIndexOf(right.grainRoom))
        {
            const int order = compareTimeGrains(schedule.timeGrains[posI],
                                                schedule.timeGrains[posJ]);
            if (order != 0)
            {
                const int firstStart = order < 0 ? startI : startJ;
                const int firstDuration = order < 0 ? durationI : durationJ;
                const int secondStart = order < 0 ? startJ : startI;
                if (secondStart - firstDuration - firstStart <= 2)
                    b.roomStabilityViolations += arc.attendanceWeight;
            }
        }
    }
    return b;
}

Score ScoreCalculator::calculateScore(MeetingSchedule &schedule)
{
    const Score score = toScore(computeBreakdown(schedule));
    schedule.score = score;
    return score;
}

}  // namespace ms
