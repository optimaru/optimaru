#include "st/bit_ops.hpp"
#include "st/canonical_constraints.hpp"
#include "st/canonical_score_calculator.hpp"
#include "st/objective_weights.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace st::canon
{

namespace
{

void fail(const std::string &message)
{
    throw std::invalid_argument("st::canon::prepare: " + message);
}

// The hard default weights, read off the components (decision: the
// monolithic totals read kDefaultWeight instead of re-typing the numbers).
// Integral, so the hard total stays integer arithmetic.
template <typename Component> constexpr std::int64_t hardWeight()
{
    static_assert(Component::kDefaultLevel == optimaru::Level::Hard,
                  "a hard total term must be a hard constraint");
    static_assert(static_cast<double>(
                      static_cast<std::int64_t>(Component::kDefaultWeight))
                      == Component::kDefaultWeight,
                  "the monolithic calculator assumes integral hard weights");
    return static_cast<std::int64_t>(Component::kDefaultWeight);
}

}  // namespace

std::int64_t Breakdown::hardTotal() const
{
    return hardWeight<UnassignedLessonsCost>() * unassignedLessonsCost
           + hardWeight<ClassConflictCost>() * classConflictCost
           + hardWeight<TeacherConflictCost>() * teacherConflictCost
           + hardWeight<DisallowedSlotCost>() * disallowedSlotCost
           + hardWeight<RoomCapacityCost>() * roomCapacityCost
           + hardWeight<CourseMaxPeriodsPerDayCost>()
                 * courseMaxPeriodsPerDayCost
           + hardWeight<BlockAdjacencyCost>() * blockAdjacencyCost
           + hardWeight<TeacherMaxPeriodsCost>() * teacherMaxPeriodsCost
           + hardWeight<TeacherMaxConsecutivePeriodsCost>()
                 * teacherMaxConsecutivePeriodsCost
           + hardWeight<FixedSlotsCost>() * fixedSlotsCost
           + hardWeight<ParallelGroupsCost>() * parallelGroupsCost;
}

double Breakdown::softPenalty(const ObjectiveWeights &w) const
{
    return w.teacherGapCost * static_cast<double>(teacherGapCost)
           + w.teacherWorkdayCost * static_cast<double>(teacherWorkdayCost)
           + w.teacherFreeDayCost * static_cast<double>(teacherFreeDayCost)
           + w.classGapCost * static_cast<double>(classGapCost)
           + w.avoidedPeriodCost * static_cast<double>(avoidedPeriodCost)
           + w.nonPreferredPeriodCost
                 * static_cast<double>(nonPreferredPeriodCost);
}

Score Breakdown::toScore(const ObjectiveWeights &w) const
{
    return {-hardTotal(), -softPenalty(w)};
}

std::int64_t
ScoreCalculator::blockViolation(std::vector<std::pair<int, int>> &dayPeriods,
                                std::size_t groupSize)
{
    if (dayPeriods.empty())
        return 0;  // nothing placed: unassignedLessonsCost charges for it
    const std::int64_t missing = static_cast<std::int64_t>(
        groupSize > dayPeriods.size() ? groupSize - dayPeriods.size() : 0);
    if (dayPeriods.size() < 2)
        return missing;
    std::sort(dayPeriods.begin(), dayPeriods.end());
    std::int64_t adjacent = 0;
    for (std::size_t i = 1; i < dayPeriods.size(); ++i)
    {
        if (dayPeriods[i].first == dayPeriods[i - 1].first
            && dayPeriods[i].second == dayPeriods[i - 1].second + 1)
        {
            ++adjacent;
        }
    }
    return static_cast<std::int64_t>(dayPeriods.size()) - 1 - adjacent
           + missing;
}

void ScoreCalculator::prepare(Timetable &table)
{
    if (table.prepared)
        return;

    const int slotCount = table.slotCount();
    if (table.periodOfSlot.size() != table.dayOfSlot.size())
        fail("dayOfSlot and periodOfSlot must have the same length");

    // Slot geometry: derive dayCount / maxPeriod, validate ranges.
    table.dayCount = 0;
    table.maxPeriod = 0;
    for (int s = 0; s < slotCount; ++s)
    {
        const int day = table.dayOfSlot[s];
        const int period = table.periodOfSlot[s];
        if (day < 0)
            fail("dayOfSlot must be >= 0");
        if (period < 1)
            fail("periodOfSlot must be >= 1 (1-based period of the day)");
        table.dayCount = std::max(table.dayCount, day + 1);
        table.maxPeriod = std::max(table.maxPeriod, period);
    }
    if (table.maxPeriod > 16)
        fail("periods per day must fit a 16-bit mask (period <= 16)");
    if (table.dayCount > 8)
        fail("days must fit an 8-bit mask (day < 8)");

    // (day, period) -> slot, rejecting duplicates.
    table.slotOfDayPeriod.assign(
        static_cast<std::size_t>(table.dayCount) * table.maxPeriod, -1);
    for (int s = 0; s < slotCount; ++s)
    {
        int &cell
            = table.slotOfDayPeriod[static_cast<std::size_t>(table.dayOfSlot[s])
                                        * table.maxPeriod
                                    + table.periodOfSlot[s] - 1];
        if (cell != -1)
            fail("duplicate (day, period) slot");
        cell = s;
    }

    // Derive id ranges from the lessons.
    int maxTeacher = table.teacherCount - 1;
    int maxClass = table.classCount - 1;
    int maxCourse = static_cast<int>(table.courseDailyLimit.size()) - 1;
    maxCourse = std::max(
        maxCourse, static_cast<int>(table.courseAvoidedPeriodMask.size()) - 1);
    maxCourse = std::max(
        maxCourse,
        static_cast<int>(table.coursePreferredPeriodMask.size()) - 1);
    int maxBlockGroup = -1;
    std::size_t maxRoomTypes = table.roomCapacityByType.size();
    for (const Lesson &lesson : table.lessons)
    {
        if (lesson.classGroupId < 0)
            fail("lesson '" + lesson.id + "' needs a classGroupId");
        if (lesson.courseKey < 0)
            fail("lesson '" + lesson.id + "' needs a courseKey");
        if (lesson.slot >= slotCount)
            fail("lesson '" + lesson.id + "' assigned outside the slot range");
        if (!lesson.allowedSlot.empty()
            && static_cast<int>(lesson.allowedSlot.size()) != slotCount)
        {
            fail("lesson '" + lesson.id
                 + "' allowedSlot must be empty or sized slotCount");
        }
        for (int t : lesson.teachers)
        {
            if (t < 0)
                fail("lesson '" + lesson.id + "' has a negative teacher id");
            maxTeacher = std::max(maxTeacher, t);
        }
        maxClass = std::max(maxClass, lesson.classGroupId);
        maxCourse = std::max(maxCourse, lesson.courseKey);
        maxBlockGroup = std::max(maxBlockGroup, lesson.blockGroup);
        maxRoomTypes = std::max(maxRoomTypes, lesson.roomTypeDemand.size());
    }
    table.teacherCount = maxTeacher + 1;
    table.classCount = maxClass + 1;
    table.courseCount = maxCourse + 1;
    table.blockGroupCount = maxBlockGroup + 1;
    table.roomTypeCount = static_cast<int>(maxRoomTypes);

    // Size the per-id vectors with the CP-SAT defaults (Teacher:
    // max_periods_per_day=6, max_consecutive_periods=4, unlimited weekly;
    // Course: max_periods_per_day=1; no period preferences; capacity 0).
    table.teacherMaxPerDay.resize(table.teacherCount, 6);
    table.teacherMaxPerWeek.resize(table.teacherCount, -1);
    table.teacherMaxConsecutive.resize(table.teacherCount, 4);
    table.teacherPreferredFreeDays.resize(table.teacherCount, 0);
    table.courseDailyLimit.resize(table.courseCount, 1);
    table.courseAvoidedPeriodMask.resize(table.courseCount, 0);
    table.coursePreferredPeriodMask.resize(table.courseCount, 0);
    table.roomCapacityByType.resize(table.roomTypeCount, 0);

    // Normalize per-lesson vectors to fixed sizes for the scoring hot paths.
    for (Lesson &lesson : table.lessons)
    {
        lesson.roomTypeDemand.resize(table.roomTypeCount, 0);
        if (lesson.allowedSlot.empty())
            lesson.allowedSlot.assign(slotCount, 1);
        // Default avoided/preferred-period unit: the courseKey's period
        // masks. Merged lessons arrive with one explicit unit per member
        // course instead.
        if (lesson.preferenceUnits.empty())
        {
            lesson.preferenceUnits.emplace_back(
                table.courseAvoidedPeriodMask[lesson.courseKey],
                table.coursePreferredPeriodMask[lesson.courseKey]);
        }
    }

    // Block-group member lists (blockAdjacencyCost recomputes one group at
    // a time).
    table.blockMembers.assign(table.blockGroupCount, {});
    for (int i = 0; i < static_cast<int>(table.lessons.size()); ++i)
    {
        const int group = table.lessons[i].blockGroup;
        if (group >= 0)
            table.blockMembers[group].push_back(i);
    }

    table.prepared = true;
}

Breakdown ScoreCalculator::computeBreakdown(Timetable &table)
{
    prepare(table);
    Breakdown b;
    const int slotCount = table.slotCount();
    const int dayCount = table.dayCount;

    // Occupancy counters (the incremental calculator maintains the same
    // shapes move by move).
    std::vector<int> classSlotCnt(
        static_cast<std::size_t>(table.classCount) * slotCount, 0);
    std::vector<int> teacherSlotCnt(
        static_cast<std::size_t>(table.teacherCount) * slotCount, 0);
    std::vector<int> roomDemand(
        static_cast<std::size_t>(table.roomTypeCount) * slotCount, 0);
    std::vector<int> courseDayCnt(
        static_cast<std::size_t>(table.courseCount) * dayCount, 0);
    std::vector<std::uint16_t> teacherDayMask(
        static_cast<std::size_t>(table.teacherCount) * dayCount, 0);
    std::vector<std::uint16_t> classDayMask(
        static_cast<std::size_t>(table.classCount) * dayCount, 0);

    for (const Lesson &lesson : table.lessons)
    {
        if (lesson.slot < 0)
        {
            ++b.unassignedLessonsCost;
            continue;
        }
        const int s = lesson.slot;
        const int day = table.dayOfSlot[s];
        const std::uint16_t bit
            = static_cast<std::uint16_t>(1U << (table.periodOfSlot[s] - 1));

        if (lesson.allowedSlot[s] == 0)
            ++b.disallowedSlotCost;

        classSlotCnt[static_cast<std::size_t>(lesson.classGroupId) * slotCount
                     + s]
            += 1;
        classDayMask[static_cast<std::size_t>(lesson.classGroupId) * dayCount
                     + day]
            |= bit;
        for (int t : lesson.teachers)
        {
            teacherSlotCnt[static_cast<std::size_t>(t) * slotCount + s] += 1;
            teacherDayMask[static_cast<std::size_t>(t) * dayCount + day] |= bit;
        }
        for (int type = 0; type < table.roomTypeCount; ++type)
        {
            roomDemand[static_cast<std::size_t>(type) * slotCount + s]
                += lesson.roomTypeDemand[type];
        }
        courseDayCnt[static_cast<std::size_t>(lesson.courseKey) * dayCount
                     + day]
            += 1;

        // avoidedPeriodCost / nonPreferredPeriodCost: every preference unit
        // (one per member course) counts on its own, so merged lessons
        // penalize once per member like validate.py.
        for (const auto &[avoided, preferred] : lesson.preferenceUnits)
        {
            if ((avoided & bit) != 0)
                ++b.avoidedPeriodCost;
            if (preferred != 0 && (preferred & bit) == 0)
                ++b.nonPreferredPeriodCost;
        }
    }

    // classConflictCost / teacherConflictCost: pairs per occupancy cell.
    for (int c : classSlotCnt)
        b.classConflictCost += pairs(c);
    for (int c : teacherSlotCnt)
        b.teacherConflictCost += pairs(c);

    // roomCapacityCost: demand excess per (roomType, slot).
    for (int type = 0; type < table.roomTypeCount; ++type)
    {
        const int capacity = table.roomCapacityByType[type];
        const std::size_t base = static_cast<std::size_t>(type) * slotCount;
        for (int s = 0; s < slotCount; ++s)
            b.roomCapacityCost += max0(roomDemand[base + s] - capacity);
    }

    // courseMaxPeriodsPerDayCost: per-course daily excess.
    for (int course = 0; course < table.courseCount; ++course)
    {
        const int limit = table.courseDailyLimit[course];
        const std::size_t base = static_cast<std::size_t>(course) * dayCount;
        for (int day = 0; day < dayCount; ++day)
            b.courseMaxPeriodsPerDayCost
                += max0(courseDayCnt[base + day] - limit);
    }

    // blockAdjacencyCost: block groups.
    std::vector<std::pair<int, int>> dayPeriods;
    for (const std::vector<int> &members : table.blockMembers)
    {
        dayPeriods.clear();
        for (int i : members)
        {
            const int s = table.lessons[i].slot;
            if (s >= 0)
                dayPeriods.emplace_back(table.dayOfSlot[s],
                                        table.periodOfSlot[s]);
        }
        b.blockAdjacencyCost += blockViolation(dayPeriods, members.size());
    }

    // The teacher-mask family from the per-(teacher, day) busy masks.
    for (int t = 0; t < table.teacherCount; ++t)
    {
        const std::size_t base = static_cast<std::size_t>(t) * dayCount;
        int weekly = 0;
        for (int day = 0; day < dayCount; ++day)
        {
            const unsigned mask = teacherDayMask[base + day];
            if (mask == 0)
                continue;
            const int busy = st::bits::popcount(mask);
            weekly += busy;
            b.teacherMaxPeriodsCost += max0(busy - table.teacherMaxPerDay[t]);
            b.teacherMaxConsecutivePeriodsCost
                += runExcessOf(mask, table.teacherMaxConsecutive[t]);
            b.teacherGapCost += gapOf(mask);
            ++b.teacherWorkdayCost;
            if ((table.teacherPreferredFreeDays[t] & (1U << day)) != 0)
                ++b.teacherFreeDayCost;
        }
        if (table.teacherMaxPerWeek[t] >= 0)
        {
            b.teacherMaxPeriodsCost
                += max0(weekly - table.teacherMaxPerWeek[t]);
        }
    }

    // classGapCost from the per-(class, day) busy masks.
    for (std::uint16_t mask : classDayMask)
        b.classGapCost += gapOf(mask);

    // fixedSlotsCost / parallelGroupsCost stay 0: pinned lessons never move
    // and a merged parallel group is one lesson.

    return b;
}

Score ScoreCalculator::calculateScore(Timetable &table)
{
    return calculateScore(table, ObjectiveWeights{});
}

Score ScoreCalculator::calculateScore(Timetable &table,
                                      const ObjectiveWeights &weights)
{
    const Breakdown b = computeBreakdown(table);
    table.score = b.toScore(weights);
    return table.score;
}

}  // namespace st::canon
