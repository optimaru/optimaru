#include "ms/incremental_calculator.hpp"

#include <cstddef>

#include "ms/score_calculator.hpp"

namespace ms
{

void MeetingSchedulingIncrementalScoreCalculator::resetWorkingSolution(
    MeetingSchedule &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    schedule_ = &workingSolution;
    span_ = workingSolution.occupancySpan;
    roomCount_ = workingSolution.roomCount();

    const int personCount = static_cast<int>(workingSolution.people.size());
    roomGrainCnt_.assign(static_cast<std::size_t>(roomCount_) * span_, 0);
    personReqCnt_.assign(static_cast<std::size_t>(personCount) * span_, 0);
    personPrefCnt_.assign(static_cast<std::size_t>(personCount) * span_, 0);
    anyGrainCnt_.assign(span_, 0);
    startCnt_.assign(span_ + 1, 0);
    endCnt_.assign(span_ + 1, 0);

    roomConflict_ = 0;
    overtime_ = 0;
    reqConflict_ = 0;
    capacity_ = 0;
    sameDay_ = 0;
    reqPref_ = 0;
    prefConflict_ = 0;
    asap_ = 0;
    breaks_ = 0;
    overlapping_ = 0;
    largerRooms_ = 0;
    stability_ = 0;

    const int assignmentCount
        = static_cast<int>(workingSolution.meetingAssignments.size());
    assign_.assign(assignmentCount, -1);
    posOf_.assign(assignmentCount, -1);
    roomOf_.assign(assignmentCount, -1);

    const int grainCount = workingSolution.timeGrainCount();
    dayOfPos_.resize(grainCount);
    minuteOfPos_.resize(grainCount);
    grainIndexOfPos_.resize(grainCount);
    for (int pos = 0; pos < grainCount; ++pos)
    {
        const TimeGrain &grain = workingSolution.timeGrains[pos];
        dayOfPos_[pos] = grain.dayOfYear;
        minuteOfPos_[pos] = grain.startingMinuteOfDay;
        grainIndexOfPos_[pos] = grain.grainIndex;
    }
    const int meetingCount = static_cast<int>(workingSolution.meetings.size());
    durationOf_.resize(meetingCount);
    for (int m = 0; m < meetingCount; ++m)
        durationOf_[m] = workingSolution.meetings[m].durationInGrains;

    for (int i = 0; i < assignmentCount; ++i)
    {
        const int grainRoom = workingSolution.meetingAssignments[i].grainRoom;
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
        roomConflict_ += roomSum;
        overlapping_ += anySum;
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
        reqConflict_ += reqSum;
        reqPref_ += prefSum;
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
        prefConflict_ += prefSum;
        reqPref_ += reqSum;
    }

    // soft 9: back-to-back pairs with meetings ending right before us or
    // starting right after us (own marks not yet counted; a meeting cannot
    // pair with itself since lastOffset + 1 > startOffset).
    if (startOffset > 0)
        breaks_ += endCnt_[startOffset - 1];
    breaks_ += startCnt_[lastOffset + 1];
    ++startCnt_[startOffset];
    ++endCnt_[lastOffset];

    // hard 2, 4, 5 + soft 8, 11: unary lookups.
    const int lastIndex = startIndex + duration - 1;
    if (s.grainCountAtOffset(lastOffset) == 0)
        overtime_ += lastIndex;
    const int shortage
        = s.requiredCapacityOf[assignment.meeting] - s.rooms[room].capacity;
    if (shortage > 0)
        capacity_ += shortage;
    sameDay_ += s.grainCountAtOffset(lastOffset)
                - s.grainCountAtOffsetOnDay(lastOffset, dayOfPos_[pos]);
    asap_ += lastIndex;
    largerRooms_ += s.largerRoomPenalty[room];

    // soft 12: room stability against the assigned shared-attendee
    // neighbors of this meeting.
    for (const auto &[otherMeeting, weight] :
         s.stabilityNeighbors[assignment.meeting])
    {
        const int j = s.assignmentOfMeeting[otherMeeting];
        if (j < 0 || assign_[j] < 0)
            continue;
        if (stabilityPairMatches(pos,
                                 room,
                                 assignment.meeting,
                                 posOf_[j],
                                 roomOf_[j],
                                 otherMeeting))
        {
            stability_ += weight;
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
         s.stabilityNeighbors[assignment.meeting])
    {
        const int j = s.assignmentOfMeeting[otherMeeting];
        if (j < 0 || assign_[j] < 0)
            continue;
        if (stabilityPairMatches(pos,
                                 room,
                                 assignment.meeting,
                                 posOf_[j],
                                 roomOf_[j],
                                 otherMeeting))
        {
            stability_ -= weight;
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
        prefConflict_ -= prefSum - duration;
        reqPref_ -= reqSum;
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
        reqConflict_ -= reqSum - duration;
        reqPref_ -= prefSum;
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
        roomConflict_ -= roomSum - duration;
        overlapping_ -= anySum - duration;
    }

    --startCnt_[startOffset];
    --endCnt_[lastOffset];
    if (startOffset > 0)
        breaks_ -= endCnt_[startOffset - 1];
    breaks_ -= startCnt_[lastOffset + 1];

    const int lastIndex = startIndex + duration - 1;
    if (s.grainCountAtOffset(lastOffset) == 0)
        overtime_ -= lastIndex;
    const int shortage
        = s.requiredCapacityOf[assignment.meeting] - s.rooms[room].capacity;
    if (shortage > 0)
        capacity_ -= shortage;
    sameDay_ -= s.grainCountAtOffset(lastOffset)
                - s.grainCountAtOffsetOnDay(lastOffset, dayOfPos_[pos]);
    asap_ -= lastIndex;
    largerRooms_ -= s.largerRoomPenalty[room];
}

bool MeetingSchedulingIncrementalScoreCalculator::stabilityPairMatches(
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

void MeetingSchedulingIncrementalScoreCalculator::beforeVariableChanged(
    int assignmentIndex)
{
    const int old = assign_[assignmentIndex];
    if (old >= 0)
        retract(assignmentIndex, old);  // unassigned: nothing to retract
    assign_[assignmentIndex] = -1;
}

void MeetingSchedulingIncrementalScoreCalculator::afterVariableChanged(
    int assignmentIndex)
{
    const int now
        = schedule_->meetingAssignments[assignmentIndex].grainRoom;  // new
    assign_[assignmentIndex] = now;
    if (now >= 0)
        insert(assignmentIndex, now);  // still unassigned: nothing to insert
}

Score MeetingSchedulingIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard
        = -(roomConflict_ + overtime_ + reqConflict_ + capacity_ + sameDay_);
    const std::int64_t medium = reqPref_ + prefConflict_;
    const std::int64_t soft
        = asap_ + 100 * breaks_ + 10 * overlapping_ + largerRooms_ + stability_;
    score.soft = -(static_cast<double>(medium) * kMediumScale
                   + static_cast<double>(soft));
    return score;
}

}  // namespace ms
