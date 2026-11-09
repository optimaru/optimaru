#include "ms/incremental_calculator.hpp"

#include <cstddef>

#include "ms/constraints.hpp"
#include "ms/score_calculator.hpp"

namespace ms
{

void MeetingSchedulingIncrementalScoreCalculator::rebuild(
    MeetingSchedule &solution)
{
    ScoreCalculator::prepare(solution);
    schedule_ = &solution;
    span_ = solution.occupancySpan;
    roomCount_ = solution.roomCount();

    const int personCount = static_cast<int>(solution.people.size());
    roomGrainCnt_.assign(static_cast<std::size_t>(roomCount_) * span_, 0);
    personReqCnt_.assign(static_cast<std::size_t>(personCount) * span_, 0);
    personPrefCnt_.assign(static_cast<std::size_t>(personCount) * span_, 0);
    anyGrainCnt_.assign(span_, 0);
    startCnt_.assign(span_ + 1, 0);
    endCnt_.assign(span_ + 1, 0);

    roomConflictCost_ = 0;
    overtimeCost_ = 0;
    requiredAttendanceConflictCost_ = 0;
    roomCapacityCost_ = 0;
    dayCrossingCost_ = 0;
    requiredAndPreferredAttendanceConflictCost_ = 0;
    preferredAttendanceConflictCost_ = 0;
    meetingEndTimeCost_ = 0;
    consecutiveMeetingsCost_ = 0;
    meetingOverlapCost_ = 0;
    largerRoomCapacityCost_ = 0;
    roomChangeCost_ = 0;

    const int assignmentCount
        = static_cast<int>(solution.meetingAssignments.size());
    assign_.assign(assignmentCount, -1);
    posOf_.assign(assignmentCount, -1);
    roomOf_.assign(assignmentCount, -1);

    const int grainCount = solution.timeGrainCount();
    dayOfPos_.resize(grainCount);
    minuteOfPos_.resize(grainCount);
    grainIndexOfPos_.resize(grainCount);
    for (int pos = 0; pos < grainCount; ++pos)
    {
        const TimeGrain &grain = solution.timeGrains[pos];
        dayOfPos_[pos] = grain.dayOfYear;
        minuteOfPos_[pos] = grain.startingMinuteOfDay;
        grainIndexOfPos_[pos] = grain.grainIndex;
    }
    const int meetingCount = static_cast<int>(solution.meetings.size());
    durationOf_.resize(meetingCount);
    for (int m = 0; m < meetingCount; ++m)
        durationOf_[m] = solution.meetings[m].durationInGrains;

    for (int i = 0; i < assignmentCount; ++i)
    {
        const int grainRoom = solution.meetingAssignments[i].grainRoom;
        assign_[i] = grainRoom;
        if (grainRoom >= 0)
            insert(i, grainRoom);
    }
}

// Adds assignment `assignmentIndex` at `grainRoom` to every counter,
// accumulating the pair deltas against the OTHER members already counted.
// All counts are read before the assignment's own increment, so each delta
// is exactly the number of new pairs (or overlap grains) it forms.
void MeetingSchedulingIncrementalScoreCalculator::insert(int assignmentIndex,
                                                         int grainRoom)
{
    const MeetingSchedule &s = *schedule_;
    const MeetingAssignment &assignment = s.meetingAssignments[assignmentIndex];
    const Meeting &meeting = s.meetings[assignment.meeting];
    const int pos = grainRoom / roomCount_;
    const int room = grainRoom - pos * roomCount_;
    const int startIndex = grainIndexOfPos_[pos];
    const int duration = durationOf_[assignment.meeting];
    const int startOffset = startIndex - s.grainIndexBase;
    const int lastOffset = startOffset + duration - 1;
    posOf_[assignmentIndex] = pos;
    roomOf_[assignmentIndex] = room;

    // The counter loops below are all split into a pure sum pass over the
    // occupied interval followed by a pure increment pass (per attendee, so a
    // person listed twice still sees their first pass's marks — the Java
    // same-meeting join). Same integer arithmetic, but each pass is a
    // straight-line loop over contiguous int cells the compiler vectorizes.

    // hard 1 + soft 10: per occupied offset, pair deltas against the other
    // meetings occupying the same (room,) offset.
    {
        int *roomCells = roomGrainCnt_.data() + roomGrainOffset(room, 0);
        int *anyCells = anyGrainCnt_.data();
        std::int64_t roomSum = 0;
        std::int64_t anySum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            roomSum += roomCells[off];
            anySum += anyCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            ++roomCells[off];
            ++anyCells[off];
        }
        roomConflictCost_ += roomSum;
        meetingOverlapCost_ += anySum;
    }

    // hard 3 + medium 6 phase 1 (required attendances): counting required
    // occupancy first means the preferred phase below sees this meeting's
    // own required marks — reproducing Java's same-meeting (required,
    // preferred) join.
    for (int person : meeting.requiredAttendees)
    {
        int *reqCells = personReqCnt_.data() + personOffset(person, 0);
        const int *prefCells = personPrefCnt_.data() + personOffset(person, 0);
        std::int64_t reqSum = 0;
        std::int64_t prefSum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            reqSum += reqCells[off];
            prefSum += prefCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
            ++reqCells[off];
        requiredAttendanceConflictCost_ += reqSum;
        requiredAndPreferredAttendanceConflictCost_ += prefSum;
    }
    // medium 6 + 7 phase 2 (preferred attendances).
    for (int person : meeting.preferredAttendees)
    {
        const int *reqCells = personReqCnt_.data() + personOffset(person, 0);
        int *prefCells = personPrefCnt_.data() + personOffset(person, 0);
        std::int64_t reqSum = 0;
        std::int64_t prefSum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            prefSum += prefCells[off];
            reqSum += reqCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
            ++prefCells[off];
        preferredAttendanceConflictCost_ += prefSum;
        requiredAndPreferredAttendanceConflictCost_ += reqSum;
    }

    // soft 9: back-to-back pairs with meetings ending right before us or
    // starting right after us (own marks not yet counted; a meeting cannot
    // pair with itself since lastOffset + 1 > startOffset).
    if (startOffset > 0)
        consecutiveMeetingsCost_ += endCnt_[startOffset - 1];
    consecutiveMeetingsCost_ += startCnt_[lastOffset + 1];
    ++startCnt_[startOffset];
    ++endCnt_[lastOffset];

    // hard 2, 4, 5 + soft 8, 11: unary lookups.
    const int lastIndex = startIndex + duration - 1;
    if (s.grainCountAtOffset(lastOffset) == 0)
        overtimeCost_ += lastIndex;
    const int shortage
        = s.requiredCapacityOf[assignment.meeting] - s.rooms[room].capacity;
    if (shortage > 0)
        roomCapacityCost_ += shortage;
    dayCrossingCost_ += s.grainCountAtOffset(lastOffset)
                        - s.grainCountAtOffsetOnDay(lastOffset, dayOfPos_[pos]);
    meetingEndTimeCost_ += lastIndex;
    largerRoomCapacityCost_ += s.largerRoomPenalty[room];

    // soft 12: roomChangeCost against the assigned shared-attendee
    // neighbors of this meeting.
    for (const auto &[otherMeeting, weight] :
         s.roomChangeNeighbors[assignment.meeting])
    {
        const int j = s.assignmentOfMeeting[otherMeeting];
        if (j < 0 || assign_[j] < 0)
            continue;
        if (roomChangePairMatches(pos,
                                  room,
                                  assignment.meeting,
                                  posOf_[j],
                                  roomOf_[j],
                                  otherMeeting))
        {
            roomChangeCost_ += weight;
        }
    }
}

// Exact mirror of insert: decrement the assignment's own counts first, then
// subtract the deltas against the remaining members (preferred phase before
// required, reversing the insert order, so the same-meeting cross products
// cancel exactly).
void MeetingSchedulingIncrementalScoreCalculator::retract(int assignmentIndex,
                                                          int grainRoom)
{
    const MeetingSchedule &s = *schedule_;
    const MeetingAssignment &assignment = s.meetingAssignments[assignmentIndex];
    const Meeting &meeting = s.meetings[assignment.meeting];
    const int pos = grainRoom / roomCount_;
    const int room = grainRoom - pos * roomCount_;
    const int startIndex = grainIndexOfPos_[pos];
    const int duration = durationOf_[assignment.meeting];
    const int startOffset = startIndex - s.grainIndexBase;
    const int lastOffset = startOffset + duration - 1;

    for (const auto &[otherMeeting, weight] :
         s.roomChangeNeighbors[assignment.meeting])
    {
        const int j = s.assignmentOfMeeting[otherMeeting];
        if (j < 0 || assign_[j] < 0)
            continue;
        if (roomChangePairMatches(pos,
                                  room,
                                  assignment.meeting,
                                  posOf_[j],
                                  roomOf_[j],
                                  otherMeeting))
        {
            roomChangeCost_ -= weight;
        }
    }

    // Split sum/decrement passes; see insert. Subtracting the pre-decrement
    // sum minus `duration` equals subtracting every post-decrement value.
    for (int person : meeting.preferredAttendees)
    {
        const int *reqCells = personReqCnt_.data() + personOffset(person, 0);
        int *prefCells = personPrefCnt_.data() + personOffset(person, 0);
        std::int64_t reqSum = 0;
        std::int64_t prefSum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            prefSum += prefCells[off];
            reqSum += reqCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
            --prefCells[off];
        preferredAttendanceConflictCost_ -= prefSum - duration;
        requiredAndPreferredAttendanceConflictCost_ -= reqSum;
    }
    for (int person : meeting.requiredAttendees)
    {
        int *reqCells = personReqCnt_.data() + personOffset(person, 0);
        const int *prefCells = personPrefCnt_.data() + personOffset(person, 0);
        std::int64_t reqSum = 0;
        std::int64_t prefSum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            reqSum += reqCells[off];
            prefSum += prefCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
            --reqCells[off];
        requiredAttendanceConflictCost_ -= reqSum - duration;
        requiredAndPreferredAttendanceConflictCost_ -= prefSum;
    }

    {
        int *roomCells = roomGrainCnt_.data() + roomGrainOffset(room, 0);
        int *anyCells = anyGrainCnt_.data();
        std::int64_t roomSum = 0;
        std::int64_t anySum = 0;
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            roomSum += roomCells[off];
            anySum += anyCells[off];
        }
        for (int off = startOffset; off <= lastOffset; ++off)
        {
            --roomCells[off];
            --anyCells[off];
        }
        roomConflictCost_ -= roomSum - duration;
        meetingOverlapCost_ -= anySum - duration;
    }

    --startCnt_[startOffset];
    --endCnt_[lastOffset];
    if (startOffset > 0)
        consecutiveMeetingsCost_ -= endCnt_[startOffset - 1];
    consecutiveMeetingsCost_ -= startCnt_[lastOffset + 1];

    const int lastIndex = startIndex + duration - 1;
    if (s.grainCountAtOffset(lastOffset) == 0)
        overtimeCost_ -= lastIndex;
    const int shortage
        = s.requiredCapacityOf[assignment.meeting] - s.rooms[room].capacity;
    if (shortage > 0)
        roomCapacityCost_ -= shortage;
    dayCrossingCost_ -= s.grainCountAtOffset(lastOffset)
                        - s.grainCountAtOffsetOnDay(lastOffset, dayOfPos_[pos]);
    meetingEndTimeCost_ -= lastIndex;
    largerRoomCapacityCost_ -= s.largerRoomPenalty[room];
}

bool MeetingSchedulingIncrementalScoreCalculator::roomChangePairMatches(
    int posA, int roomA, int meetingA, int posB, int roomB, int meetingB) const
{
    if (roomA == roomB)
        return false;
    // Java TimeGrain comparator: (dayOfYear, startingMinuteOfDay), strict.
    int order = 0;
    if (dayOfPos_[posA] != dayOfPos_[posB])
        order = dayOfPos_[posA] < dayOfPos_[posB] ? -1 : 1;
    else if (minuteOfPos_[posA] != minuteOfPos_[posB])
        order = minuteOfPos_[posA] < minuteOfPos_[posB] ? -1 : 1;
    if (order == 0)
        return false;
    const int startA = grainIndexOfPos_[posA];
    const int startB = grainIndexOfPos_[posB];
    const int firstStart = order < 0 ? startA : startB;
    const int firstDuration
        = order < 0 ? durationOf_[meetingA] : durationOf_[meetingB];
    const int secondStart = order < 0 ? startB : startA;
    return secondStart - firstDuration - firstStart <= 2;
}

void MeetingSchedulingIncrementalScoreCalculator::before_assign(
    int assignmentIndex)
{
    const int old = assign_[assignmentIndex];
    if (old >= 0)
        retract(assignmentIndex, old);  // unassigned: nothing to retract
    assign_[assignmentIndex] = -1;
}

void MeetingSchedulingIncrementalScoreCalculator::after_assign(
    int assignmentIndex)
{
    const int now
        = schedule_->meetingAssignments[assignmentIndex].grainRoom;  // new
    assign_[assignmentIndex] = now;
    if (now >= 0)
        insert(assignmentIndex, now);  // still unassigned: nothing to insert
}

Score MeetingSchedulingIncrementalScoreCalculator::score()
{
    // The default assembly's weights (the components' kDefaultWeight) and the
    // domain's medium fold, like ScoreCalculator::toScore.
    const std::int64_t hard
        = kHardWeight<RoomConflictCost> * roomConflictCost_
          + kHardWeight<OvertimeCost> * overtimeCost_
          + kHardWeight<RequiredAttendanceConflictCost>
                * requiredAttendanceConflictCost_
          + kHardWeight<RoomCapacityCost> * roomCapacityCost_
          + kHardWeight<DayCrossingCost> * dayCrossingCost_;
    const std::int64_t medium
        = kMediumWeight<RequiredAndPreferredAttendanceConflictCost>
              * requiredAndPreferredAttendanceConflictCost_
          + kMediumWeight<PreferredAttendanceConflictCost>
                * preferredAttendanceConflictCost_;
    const std::int64_t soft
        = kSoftWeight<MeetingEndTimeCost> * meetingEndTimeCost_
          + kSoftWeight<ConsecutiveMeetingsCost> * consecutiveMeetingsCost_
          + kSoftWeight<MeetingOverlapCost> * meetingOverlapCost_
          + kSoftWeight<LargerRoomCapacityCost> * largerRoomCapacityCost_
          + kSoftWeight<RoomChangeCost> * roomChangeCost_;
    return kMediumFold.to_engine(
        -hard, -static_cast<double>(medium), -static_cast<double>(soft));
}

}  // namespace ms
