#include "st/bit_ops.hpp"
#include "st/canonical_incremental_calculator.hpp"

#include <cstddef>

#include "st/canonical_score_calculator.hpp"

namespace st::canon
{

void CanonicalIncrementalScoreCalculator::resetWorkingSolution(
    Timetable &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    table_ = &workingSolution;
    slotCount_ = workingSolution.slotCount();
    dayCount_ = workingSolution.dayCount;

    classSlotCnt_.assign(
        static_cast<std::size_t>(workingSolution.classCount) * slotCount_, 0);
    teacherSlotCnt_.assign(
        static_cast<std::size_t>(workingSolution.teacherCount) * slotCount_, 0);
    roomDemandCnt_.assign(
        static_cast<std::size_t>(workingSolution.roomTypeCount) * slotCount_,
        0);
    courseDayCnt_.assign(
        static_cast<std::size_t>(workingSolution.courseCount) * dayCount_, 0);
    teacherDayMask_.assign(
        static_cast<std::size_t>(workingSolution.teacherCount) * dayCount_, 0);
    classDayMask_.assign(
        static_cast<std::size_t>(workingSolution.classCount) * dayCount_, 0);
    teacherWeekCnt_.assign(workingSolution.teacherCount, 0);
    blockViol_.assign(workingSolution.blockGroupCount, 0);

    const int lessonCountForMaps
        = static_cast<int>(workingSolution.lessons.size());
    hotClassCells_.reset(classSlotCnt_.size());
    hotTeacherCells_.reset(teacherSlotCnt_.size());
    hotNotAllowed_.reset(lessonCountForMaps);
    hotRoomCells_.reset(roomDemandCnt_.size());
    hotCourseDays_.reset(courseDayCnt_.size());
    hotBlocks_.reset(blockViol_.size());
    hotTeacherDays_.reset(teacherDayMask_.size());
    hotTeachersWeek_.reset(teacherWeekCnt_.size());
    lessonsOfClass_.assign(workingSolution.classCount, {});
    lessonsOfTeacher_.assign(workingSolution.teacherCount, {});
    lessonsOfCourse_.assign(workingSolution.courseCount, {});
    lessonsOfRoomType_.assign(workingSolution.roomTypeCount, {});
    for (int i = 0; i < lessonCountForMaps; ++i)
    {
        const Lesson &lesson = workingSolution.lessons[i];
        lessonsOfClass_[lesson.classGroupId].push_back(i);
        for (int t : lesson.teachers)
            lessonsOfTeacher_[t].push_back(i);
        lessonsOfCourse_[lesson.courseKey].push_back(i);
        for (int type = 0; type < workingSolution.roomTypeCount; ++type)
        {
            if (lesson.roomTypeDemand[type] > 0)
                lessonsOfRoomType_[type].push_back(i);
        }
    }

    classPairs_ = 0;
    teacherPairs_ = 0;
    notAllowed_ = 0;
    roomExcess_ = 0;
    courseExcess_ = 0;
    blockTotal_ = 0;
    teacherDailyExcess_ = 0;
    teacherWeeklyExcess_ = 0;
    consecutiveExcess_ = 0;
    teacherGap_ = 0;
    teacherWorkday_ = 0;
    teacherFreeDay_ = 0;
    classGap_ = 0;
    avoided_ = 0;
    nonPreferred_ = 0;

    const int lessonCount = static_cast<int>(workingSolution.lessons.size());
    unassigned_ = lessonCount;
    assign_.assign(lessonCount, -1);
    for (int i = 0; i < lessonCount; ++i)
    {
        const int slot = workingSolution.lessons[i].slot;
        if (slot >= 0)
        {
            assign_[i] = slot;
            insert(i, slot);
        }
    }
}

void CanonicalIncrementalScoreCalculator::setTeacherDayMask(
    int teacher, int day, std::uint16_t newMask)
{
    std::uint16_t &mask = teacherDayMask_[dayOffset(teacher, day)];
    const std::uint16_t oldMask = mask;
    mask = newMask;

    const int maxPerDay = table_->teacherMaxPerDay[teacher];
    const int maxConsecutive = table_->teacherMaxConsecutive[teacher];
    const int oldBusy = st::bits::popcount(oldMask);
    const int newBusy = st::bits::popcount(newMask);

    const std::int64_t newDaily = ScoreCalculator::max0(newBusy - maxPerDay);
    const std::int64_t newRun
        = ScoreCalculator::runExcessOf(newMask, maxConsecutive);
    teacherDailyExcess_
        += newDaily - ScoreCalculator::max0(oldBusy - maxPerDay);
    consecutiveExcess_
        += newRun - ScoreCalculator::runExcessOf(oldMask, maxConsecutive);
    hotTeacherDays_.set(static_cast<int>(dayOffset(teacher, day)),
                        newDaily > 0 || newRun > 0);
    teacherGap_
        += ScoreCalculator::gapOf(newMask) - ScoreCalculator::gapOf(oldMask);
    const int workdayDelta = (newMask != 0 ? 1 : 0) - (oldMask != 0 ? 1 : 0);
    teacherWorkday_ += workdayDelta;
    if ((table_->teacherPreferredFreeDays[teacher] & (1U << day)) != 0)
        teacherFreeDay_ += workdayDelta;

    const int maxPerWeek = table_->teacherMaxPerWeek[teacher];
    const int oldWeek = teacherWeekCnt_[teacher];
    const int newWeek = oldWeek + newBusy - oldBusy;
    teacherWeekCnt_[teacher] = newWeek;
    if (maxPerWeek >= 0)
    {
        teacherWeeklyExcess_ += ScoreCalculator::max0(newWeek - maxPerWeek)
                                - ScoreCalculator::max0(oldWeek - maxPerWeek);
        hotTeachersWeek_.set(teacher, newWeek > maxPerWeek);
    }
}

void CanonicalIncrementalScoreCalculator::setClassDayMask(int classGroup,
                                                          int day,
                                                          std::uint16_t newMask)
{
    std::uint16_t &mask = classDayMask_[dayOffset(classGroup, day)];
    classGap_ += ScoreCalculator::gapOf(newMask) - ScoreCalculator::gapOf(mask);
    mask = newMask;
}

void CanonicalIncrementalScoreCalculator::refreshBlock(int group)
{
    blockScratch_.clear();
    for (int i : table_->blockMembers[group])
    {
        const int s = assign_[i];
        if (s >= 0)
            blockScratch_.emplace_back(table_->dayOfSlot[s],
                                       table_->periodOfSlot[s]);
    }
    const std::int64_t violation = ScoreCalculator::blockViolation(
        blockScratch_, table_->blockMembers[group].size());
    blockTotal_ += violation - blockViol_[group];
    blockViol_[group] = violation;
    hotBlocks_.set(group, violation > 0);
}

// Adds lesson `lessonIndex` at `slot` to every counter. Counts are read
// before the lesson's own increment, so each pair delta is exactly the number
// of new pairs the lesson forms; excess terms subtract the old from the new
// closed-form value. Requires assign_[lessonIndex] == slot (refreshBlock
// reads it).
void CanonicalIncrementalScoreCalculator::insert(int lessonIndex, int slot)
{
    const Lesson &lesson = table_->lessons[lessonIndex];
    const int day = table_->dayOfSlot[slot];
    const std::uint16_t bit
        = static_cast<std::uint16_t>(1U << (table_->periodOfSlot[slot] - 1));

    --unassigned_;  // H1

    if (lesson.allowedSlot[slot] == 0)
    {
        ++notAllowed_;  // H5
        hotNotAllowed_.set(lessonIndex, true);
    }

    // H3 + S4: class occupancy (one unit per merged lesson).
    {
        const std::size_t cell = slotOffset(lesson.classGroupId, slot);
        int &cnt = classSlotCnt_[cell];
        classPairs_ += cnt++;
        if (cnt == 1)
        {
            setClassDayMask(lesson.classGroupId,
                            day,
                            classDayMask_[dayOffset(lesson.classGroupId, day)]
                                | bit);
        }
        else if (cnt == 2)
        {
            hotClassCells_.set(static_cast<int>(cell), true);
        }
    }

    // H4 + H9/H10 + S1~S3: every teacher of the lesson occupies the slot.
    for (int t : lesson.teachers)
    {
        const std::size_t cell = slotOffset(t, slot);
        int &cnt = teacherSlotCnt_[cell];
        teacherPairs_ += cnt++;
        if (cnt == 1)
        {
            setTeacherDayMask(t, day, teacherDayMask_[dayOffset(t, day)] | bit);
        }
        else if (cnt == 2)
        {
            hotTeacherCells_.set(static_cast<int>(cell), true);
        }
    }

    // H6: room-demand excess per (type, slot).
    for (int type = 0; type < table_->roomTypeCount; ++type)
    {
        const int demand = lesson.roomTypeDemand[type];
        if (demand == 0)
            continue;
        const std::size_t cell = slotOffset(type, slot);
        int &cnt = roomDemandCnt_[cell];
        const int capacity = table_->roomCapacityByType[type];
        roomExcess_ += ScoreCalculator::max0(cnt + demand - capacity)
                       - ScoreCalculator::max0(cnt - capacity);
        cnt += demand;
        hotRoomCells_.set(static_cast<int>(cell), cnt > capacity);
    }

    // H7: per-course daily excess.
    {
        const std::size_t cell = dayOffset(lesson.courseKey, day);
        int &cnt = courseDayCnt_[cell];
        const int limit = table_->courseDailyLimit[lesson.courseKey];
        courseExcess_ += ScoreCalculator::max0(cnt + 1 - limit)
                         - ScoreCalculator::max0(cnt - limit);
        ++cnt;
        hotCourseDays_.set(static_cast<int>(cell), cnt > limit);
    }

    // S5/S6: one hit per preference unit (per member course of a merged
    // lesson).
    for (const auto &[avoided, preferred] : lesson.preferenceUnits)
    {
        if ((avoided & bit) != 0)
            ++avoided_;
        if (preferred != 0 && (preferred & bit) == 0)
            ++nonPreferred_;
    }

    // H8: recompute the whole (small) block group.
    if (lesson.blockGroup >= 0)
        refreshBlock(lesson.blockGroup);
}

// Exact mirror of insert. Requires assign_[lessonIndex] == -1 already
// (refreshBlock must no longer see the lesson).
void CanonicalIncrementalScoreCalculator::retract(int lessonIndex, int slot)
{
    const Lesson &lesson = table_->lessons[lessonIndex];
    const int day = table_->dayOfSlot[slot];
    const std::uint16_t bit
        = static_cast<std::uint16_t>(1U << (table_->periodOfSlot[slot] - 1));

    ++unassigned_;  // H1

    if (lesson.allowedSlot[slot] == 0)
    {
        --notAllowed_;  // H5
        hotNotAllowed_.set(lessonIndex, false);
    }

    {
        const std::size_t cell = slotOffset(lesson.classGroupId, slot);
        int &cnt = classSlotCnt_[cell];
        classPairs_ -= --cnt;
        if (cnt == 0)
        {
            setClassDayMask(lesson.classGroupId,
                            day,
                            classDayMask_[dayOffset(lesson.classGroupId, day)]
                                & static_cast<std::uint16_t>(~bit));
        }
        else if (cnt == 1)
        {
            hotClassCells_.set(static_cast<int>(cell), false);
        }
    }

    for (int t : lesson.teachers)
    {
        const std::size_t cell = slotOffset(t, slot);
        int &cnt = teacherSlotCnt_[cell];
        teacherPairs_ -= --cnt;
        if (cnt == 0)
        {
            setTeacherDayMask(t,
                              day,
                              teacherDayMask_[dayOffset(t, day)]
                                  & static_cast<std::uint16_t>(~bit));
        }
        else if (cnt == 1)
        {
            hotTeacherCells_.set(static_cast<int>(cell), false);
        }
    }

    for (int type = 0; type < table_->roomTypeCount; ++type)
    {
        const int demand = lesson.roomTypeDemand[type];
        if (demand == 0)
            continue;
        const std::size_t cell = slotOffset(type, slot);
        int &cnt = roomDemandCnt_[cell];
        const int capacity = table_->roomCapacityByType[type];
        roomExcess_ += ScoreCalculator::max0(cnt - demand - capacity)
                       - ScoreCalculator::max0(cnt - capacity);
        cnt -= demand;
        hotRoomCells_.set(static_cast<int>(cell), cnt > capacity);
    }

    {
        const std::size_t cell = dayOffset(lesson.courseKey, day);
        int &cnt = courseDayCnt_[cell];
        const int limit = table_->courseDailyLimit[lesson.courseKey];
        courseExcess_ += ScoreCalculator::max0(cnt - 1 - limit)
                         - ScoreCalculator::max0(cnt - limit);
        --cnt;
        hotCourseDays_.set(static_cast<int>(cell), cnt > limit);
    }

    for (const auto &[avoided, preferred] : lesson.preferenceUnits)
    {
        if ((avoided & bit) != 0)
            --avoided_;
        if (preferred != 0 && (preferred & bit) == 0)
            --nonPreferred_;
    }

    if (lesson.blockGroup >= 0)
        refreshBlock(lesson.blockGroup);
}

void CanonicalIncrementalScoreCalculator::beforeVariableChanged(int lessonIndex)
{
    const int old = assign_[lessonIndex];
    assign_[lessonIndex] = -1;
    if (old >= 0)
        retract(lessonIndex, old);  // unassigned: nothing to retract
}

void CanonicalIncrementalScoreCalculator::afterVariableChanged(int lessonIndex)
{
    const int now = table_->lessons[lessonIndex].slot;  // the new value
    assign_[lessonIndex] = now;
    if (now >= 0)
        insert(lessonIndex, now);  // still unassigned: nothing to insert
}

int CanonicalIncrementalScoreCalculator::hotLesson(std::mt19937_64 &rng) const
{
    const std::size_t total = hotNotAllowed_.size() + hotClassCells_.size()
                              + hotTeacherCells_.size() + hotRoomCells_.size()
                              + hotCourseDays_.size() + hotBlocks_.size()
                              + hotTeacherDays_.size()
                              + hotTeachersWeek_.size();
    if (total == 0)
        return -1;
    std::uint64_t r = rng() % total;

    // H5: the hot key IS the lesson.
    if (r < hotNotAllowed_.size())
        return hotNotAllowed_.items()[r];
    r -= hotNotAllowed_.size();

    // H3: one of the lessons of the overfull (class, slot) cell.
    if (r < hotClassCells_.size())
    {
        const int key = hotClassCells_.items()[r];
        const int slot = key % slotCount_;
        return pickLesson(lessonsOfClass_[key / slotCount_],
                          rng,
                          [&](int i) { return assign_[i] == slot; });
    }
    r -= hotClassCells_.size();

    // H4: one of the lessons of the overfull (teacher, slot) cell.
    if (r < hotTeacherCells_.size())
    {
        const int key = hotTeacherCells_.items()[r];
        const int slot = key % slotCount_;
        return pickLesson(lessonsOfTeacher_[key / slotCount_],
                          rng,
                          [&](int i) { return assign_[i] == slot; });
    }
    r -= hotTeacherCells_.size();

    // H6: one of the demanding lessons at the over-capacity (type, slot).
    if (r < hotRoomCells_.size())
    {
        const int key = hotRoomCells_.items()[r];
        const int slot = key % slotCount_;
        return pickLesson(lessonsOfRoomType_[key / slotCount_],
                          rng,
                          [&](int i) { return assign_[i] == slot; });
    }
    r -= hotRoomCells_.size();

    // H7: one of the course's lessons on the over-limit day.
    if (r < hotCourseDays_.size())
    {
        const int key = hotCourseDays_.items()[r];
        const int day = key % dayCount_;
        return pickLesson(lessonsOfCourse_[key / dayCount_],
                          rng,
                          [&](int i)
                          { return table_->dayOfSlot[assign_[i]] == day; });
    }
    r -= hotCourseDays_.size();

    // H8: one of the broken block group's members.
    if (r < hotBlocks_.size())
    {
        return pickLesson(table_->blockMembers[hotBlocks_.items()[r]],
                          rng,
                          [](int) { return true; });
    }
    r -= hotBlocks_.size();

    // H9 daily / H10: one of the teacher's lessons on the excess day.
    if (r < hotTeacherDays_.size())
    {
        const int key = hotTeacherDays_.items()[r];
        const int day = key % dayCount_;
        return pickLesson(lessonsOfTeacher_[key / dayCount_],
                          rng,
                          [&](int i)
                          { return table_->dayOfSlot[assign_[i]] == day; });
    }
    r -= hotTeacherDays_.size();

    // H9 weekly: any lesson of the over-loaded teacher.
    return pickLesson(lessonsOfTeacher_[hotTeachersWeek_.items()[r]],
                      rng,
                      [](int) { return true; });
}

bool CanonicalIncrementalScoreCalculator::hotSetsConsistent() const
{
    for (std::size_t cell = 0; cell < classSlotCnt_.size(); ++cell)
    {
        if (hotClassCells_.contains(static_cast<int>(cell))
            != (classSlotCnt_[cell] >= 2))
            return false;
    }
    for (std::size_t cell = 0; cell < teacherSlotCnt_.size(); ++cell)
    {
        if (hotTeacherCells_.contains(static_cast<int>(cell))
            != (teacherSlotCnt_[cell] >= 2))
            return false;
    }
    for (int i = 0; i < static_cast<int>(assign_.size()); ++i)
    {
        const bool expected
            = assign_[i] >= 0
              && table_->lessons[i].allowedSlot[assign_[i]] == 0;
        if (hotNotAllowed_.contains(i) != expected)
            return false;
    }
    for (int type = 0; type < table_->roomTypeCount; ++type)
    {
        const int capacity = table_->roomCapacityByType[type];
        for (int s = 0; s < slotCount_; ++s)
        {
            const std::size_t cell = slotOffset(type, s);
            if (hotRoomCells_.contains(static_cast<int>(cell))
                != (roomDemandCnt_[cell] > capacity))
                return false;
        }
    }
    for (int course = 0; course < table_->courseCount; ++course)
    {
        const int limit = table_->courseDailyLimit[course];
        for (int day = 0; day < dayCount_; ++day)
        {
            const std::size_t cell = dayOffset(course, day);
            if (hotCourseDays_.contains(static_cast<int>(cell))
                != (courseDayCnt_[cell] > limit))
                return false;
        }
    }
    for (int group = 0; group < static_cast<int>(blockViol_.size()); ++group)
    {
        if (hotBlocks_.contains(group) != (blockViol_[group] > 0))
            return false;
    }
    for (int t = 0; t < table_->teacherCount; ++t)
    {
        for (int day = 0; day < dayCount_; ++day)
        {
            const std::size_t cell = dayOffset(t, day);
            const unsigned mask = teacherDayMask_[cell];
            const bool expected
                = ScoreCalculator::max0(st::bits::popcount(mask)
                                        - table_->teacherMaxPerDay[t])
                      > 0
                  || ScoreCalculator::runExcessOf(
                         mask, table_->teacherMaxConsecutive[t])
                         > 0;
            if (hotTeacherDays_.contains(static_cast<int>(cell)) != expected)
                return false;
        }
        const int maxPerWeek = table_->teacherMaxPerWeek[t];
        const bool expected
            = maxPerWeek >= 0 && teacherWeekCnt_[t] > maxPerWeek;
        if (hotTeachersWeek_.contains(t) != expected)
            return false;
    }
    return true;
}

Score CanonicalIncrementalScoreCalculator::calculateScore()
{
    const std::int64_t hard = unassigned_ + classPairs_ + teacherPairs_
                              + notAllowed_ + roomExcess_ + courseExcess_
                              + blockTotal_ + teacherDailyExcess_
                              + teacherWeeklyExcess_ + consecutiveExcess_;
    const ObjectiveWeights &w = table_->weights;
    const std::int64_t penalty
        = w.teacherGap * teacherGap_ + w.teacherWorkday * teacherWorkday_
          + w.teacherFreeDay * teacherFreeDay_ + w.classGap * classGap_
          + w.avoidedPeriod * avoided_ + w.nonPreferredPeriod * nonPreferred_;
    return {-hard, -static_cast<double>(penalty)};
}

}  // namespace st::canon
