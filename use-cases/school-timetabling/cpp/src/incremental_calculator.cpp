#include "st/incremental_calculator.hpp"

#include <cstddef>

#include "st/score_calculator.hpp"

namespace st
{

void SchoolTimetablingIncrementalScoreCalculator::resetWorkingSolution(
    Timetable &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    table_ = &workingSolution;
    tsCount_ = workingSolution.timeslotCount();
    roomCount_ = workingSolution.roomCount();

    slotRoomCnt_.assign(workingSolution.slotRoomCount(), 0);
    teacherSlotCnt_.assign(
        static_cast<std::size_t>(workingSolution.teacherCount) * tsCount_, 0);
    groupSlotCnt_.assign(
        static_cast<std::size_t>(workingSolution.groupCount) * tsCount_, 0);
    groupSubjectSlotCnt_.assign(
        static_cast<std::size_t>(workingSolution.groupSubjectCount) * tsCount_,
        0);
    teacherLessons_.assign(workingSolution.teacherCount, 0);
    teacherRoomCnt_.assign(
        static_cast<std::size_t>(workingSolution.teacherCount) * roomCount_, 0);

    conflictPairs_ = 0;
    unavailable_ = 0;
    stability_ = 0;
    efficiency_ = 0;
    variety_ = 0;

    assign_.assign(workingSolution.lessons.size(), -1);
    for (int i = 0; i < static_cast<int>(workingSolution.lessons.size()); ++i)
    {
        const int slotRoom = workingSolution.lessons[i].slotRoom;
        assign_[i] = slotRoom;
        if (slotRoom >= 0)
            insert(i, slotRoom);
    }
}

// Adds lesson `lessonIndex` at `slotRoom` to every counter, accumulating the
// pair/product deltas against the OTHER members already counted. All counts
// are read before the lesson's own increment, so each delta is exactly the
// number of new pairs the lesson forms.
void SchoolTimetablingIncrementalScoreCalculator::insert(int lessonIndex,
                                                         int slotRoom)
{
    const Lesson &lesson = table_->lessons[lessonIndex];
    const int ts = slotRoom / roomCount_;
    const int room = slotRoom % roomCount_;
    const int next = table_->nextSlot[ts];
    const int prev = table_->prevSlot[ts];

    // hard 1: roomConflict — the slotRoom value IS the (timeslot, room) cell.
    conflictPairs_ += slotRoomCnt_[slotRoom]++;

    // hard 2 + soft 6: teacherConflict / teacherTimeEfficiency.
    conflictPairs_ += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    if (next >= 0)
        efficiency_
            += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, next)];
    if (prev >= 0)
        efficiency_
            += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, prev)];
    ++teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];

    // hard 3: studentGroupConflict.
    conflictPairs_ += groupSlotCnt_[groupSlotOffset(lesson.groupId, ts)]++;

    // soft 7: studentGroupSubjectVariety.
    if (next >= 0)
    {
        variety_ += groupSubjectSlotCnt_[groupSubjectSlotOffset(
            lesson.groupSubjectId, next)];
    }
    if (prev >= 0)
    {
        variety_ += groupSubjectSlotCnt_[groupSubjectSlotOffset(
            lesson.groupSubjectId, prev)];
    }
    ++groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, ts)];

    // hard 4: teacherUnavailable.
    if (ScoreCalculator::teacherUnavailableAt(*table_, lesson.teacherId, ts))
        ++unavailable_;

    // soft 5: teacherRoomStability — the lesson pairs with all n current
    // lessons of its teacher, of which c_room share its room.
    stability_ += teacherLessons_[lesson.teacherId]
                  - teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
    ++teacherLessons_[lesson.teacherId];
    ++teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
}

// Exact mirror of insert: decrement the lesson's own counts first, then
// subtract the deltas against the remaining members.
void SchoolTimetablingIncrementalScoreCalculator::retract(int lessonIndex,
                                                          int slotRoom)
{
    const Lesson &lesson = table_->lessons[lessonIndex];
    const int ts = slotRoom / roomCount_;
    const int room = slotRoom % roomCount_;
    const int next = table_->nextSlot[ts];
    const int prev = table_->prevSlot[ts];

    conflictPairs_ -= --slotRoomCnt_[slotRoom];

    --teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    conflictPairs_ -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    if (next >= 0)
        efficiency_
            -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, next)];
    if (prev >= 0)
        efficiency_
            -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, prev)];

    conflictPairs_ -= --groupSlotCnt_[groupSlotOffset(lesson.groupId, ts)];

    --groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, ts)];
    if (next >= 0)
    {
        variety_ -= groupSubjectSlotCnt_[groupSubjectSlotOffset(
            lesson.groupSubjectId, next)];
    }
    if (prev >= 0)
    {
        variety_ -= groupSubjectSlotCnt_[groupSubjectSlotOffset(
            lesson.groupSubjectId, prev)];
    }

    if (ScoreCalculator::teacherUnavailableAt(*table_, lesson.teacherId, ts))
        --unavailable_;

    --teacherLessons_[lesson.teacherId];
    --teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
    stability_ -= teacherLessons_[lesson.teacherId]
                  - teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
}

void SchoolTimetablingIncrementalScoreCalculator::beforeVariableChanged(
    int lessonIndex)
{
    const int old = assign_[lessonIndex];
    if (old >= 0)
        retract(lessonIndex, old);  // unassigned: nothing to retract
    assign_[lessonIndex] = -1;
}

void SchoolTimetablingIncrementalScoreCalculator::afterVariableChanged(
    int lessonIndex)
{
    const int now = table_->lessons[lessonIndex].slotRoom;  // the new value
    assign_[lessonIndex] = now;
    if (now >= 0)
        insert(lessonIndex, now);  // now unassigned: nothing to insert
}

Score SchoolTimetablingIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(conflictPairs_ + unavailable_);
    score.soft = static_cast<double>(efficiency_ - stability_ - variety_);
    return score;
}

}  // namespace st
