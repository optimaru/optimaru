#include "st/incremental_calculator.hpp"

#include <cstddef>

#include "st/score_calculator.hpp"

namespace st {

void SchoolTimetablingIncrementalScoreCalculator::rebuild(Timetable& solution) {
    ScoreCalculator::prepare(solution);
    table_ = &solution;
    tsCount_ = solution.timeslotCount();
    roomCount_ = solution.roomCount();

    slotRoomCnt_.assign(solution.slotRoomCount(), 0);
    teacherSlotCnt_.assign(static_cast<std::size_t>(solution.teacherCount) * tsCount_, 0);
    groupSlotCnt_.assign(static_cast<std::size_t>(solution.groupCount) * tsCount_, 0);
    groupSubjectSlotCnt_.assign(static_cast<std::size_t>(solution.groupSubjectCount) * tsCount_, 0);
    teacherLessons_.assign(solution.teacherCount, 0);
    teacherRoomCnt_.assign(static_cast<std::size_t>(solution.teacherCount) * roomCount_, 0);

    conflictPairs_ = 0;
    unavailable_ = 0;
    roomChange_ = 0;
    adjacentPairs_ = 0;
    subjectRepeat_ = 0;

    assign_.assign(solution.lessons.size(), -1);
    for (int i = 0; i < static_cast<int>(solution.lessons.size()); ++i) {
        const int slotRoom = solution.lessons[i].slotRoom;
        assign_[i] = slotRoom;
        if (slotRoom >= 0) insert(i, slotRoom);
    }
}

// Adds lesson `lessonIndex` at `slotRoom` to every counter, accumulating the
// pair/product deltas against the OTHER members already counted. All counts
// are read before the lesson's own increment, so each delta is exactly the
// number of new pairs the lesson forms.
void SchoolTimetablingIncrementalScoreCalculator::insert(int lessonIndex, int slotRoom) {
    const Lesson& lesson = table_->lessons[lessonIndex];
    const int ts = slotRoom / roomCount_;
    const int room = slotRoom % roomCount_;
    const int next = table_->nextSlot[ts];
    const int prev = table_->prevSlot[ts];

    // hard 1: roomConflictCost — the slotRoom value IS the (timeslot, room)
    // cell.
    conflictPairs_ += slotRoomCnt_[slotRoom]++;

    // hard 2 + soft 6: teacherConflictCost / teacherTimeEfficiencyCost.
    conflictPairs_ += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    if (next >= 0) adjacentPairs_ += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, next)];
    if (prev >= 0) adjacentPairs_ += teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, prev)];
    ++teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];

    // hard 3: classConflictCost.
    conflictPairs_ += groupSlotCnt_[groupSlotOffset(lesson.groupId, ts)]++;

    // soft 7: classSubjectRepeatCost.
    if (next >= 0) {
        subjectRepeat_ += groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, next)];
    }
    if (prev >= 0) {
        subjectRepeat_ += groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, prev)];
    }
    ++groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, ts)];

    // hard 4: teacherUnavailabilityCost.
    if (ScoreCalculator::teacherUnavailableAt(*table_, lesson.teacherId, ts)) ++unavailable_;

    // soft 5: teacherRoomChangeCost — the lesson pairs with all n current
    // lessons of its teacher, of which c_room share its room.
    roomChange_ += teacherLessons_[lesson.teacherId] -
                   teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
    ++teacherLessons_[lesson.teacherId];
    ++teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
}

// Exact mirror of insert: decrement the lesson's own counts first, then
// subtract the deltas against the remaining members.
void SchoolTimetablingIncrementalScoreCalculator::retract(int lessonIndex, int slotRoom) {
    const Lesson& lesson = table_->lessons[lessonIndex];
    const int ts = slotRoom / roomCount_;
    const int room = slotRoom % roomCount_;
    const int next = table_->nextSlot[ts];
    const int prev = table_->prevSlot[ts];

    conflictPairs_ -= --slotRoomCnt_[slotRoom];

    --teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    conflictPairs_ -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, ts)];
    if (next >= 0) adjacentPairs_ -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, next)];
    if (prev >= 0) adjacentPairs_ -= teacherSlotCnt_[teacherSlotOffset(lesson.teacherId, prev)];

    conflictPairs_ -= --groupSlotCnt_[groupSlotOffset(lesson.groupId, ts)];

    --groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, ts)];
    if (next >= 0) {
        subjectRepeat_ -= groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, next)];
    }
    if (prev >= 0) {
        subjectRepeat_ -= groupSubjectSlotCnt_[groupSubjectSlotOffset(lesson.groupSubjectId, prev)];
    }

    if (ScoreCalculator::teacherUnavailableAt(*table_, lesson.teacherId, ts)) --unavailable_;

    --teacherLessons_[lesson.teacherId];
    --teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
    roomChange_ -= teacherLessons_[lesson.teacherId] -
                   teacherRoomCnt_[teacherRoomOffset(lesson.teacherId, room)];
}

void SchoolTimetablingIncrementalScoreCalculator::before_assign(int lessonIndex) {
    const int old = assign_[lessonIndex];
    if (old >= 0) retract(lessonIndex, old);  // unassigned: nothing to retract
    assign_[lessonIndex] = -1;
}

void SchoolTimetablingIncrementalScoreCalculator::after_assign(int lessonIndex) {
    const int now = table_->lessons[lessonIndex].slotRoom;  // the new value
    assign_[lessonIndex] = now;
    if (now >= 0) insert(lessonIndex, now);  // now unassigned: nothing to insert
}

Score SchoolTimetablingIncrementalScoreCalculator::score() {
    Score score;
    score.hard = -(conflictPairs_ + unavailable_);
    score.soft = static_cast<double>(adjacentPairs_ - roomChange_ - subjectRepeat_);
    return score;
}

}  // namespace st
