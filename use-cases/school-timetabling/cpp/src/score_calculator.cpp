#include "st/score_calculator.hpp"

#include <cstddef>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace st
{

void ScoreCalculator::prepare(Timetable &table)
{
    if (table.prepared)
        return;

    // Intern teachers, groups and (group, subject) pairs into dense ids so the
    // scoring loops index flat count arrays instead of hashing strings.
    std::unordered_map<std::string, int> teacherId;
    std::unordered_map<std::string, int> groupId;
    std::unordered_map<std::string, int> groupSubjectId;
    auto idOf
        = [](std::unordered_map<std::string, int> &ids, const std::string &key)
    {
        auto it = ids.find(key);
        if (it != ids.end())
            return it->second;
        int id = static_cast<int>(ids.size());
        ids.emplace(key, id);
        return id;
    };
    for (Lesson &lesson : table.lessons)
    {
        lesson.teacherId = idOf(teacherId, lesson.teacher);
        lesson.groupId = idOf(groupId, lesson.studentGroup);
        // '\x1f' (unit separator) cannot occur in the names, so the composite
        // key never collides across (group, subject) boundaries.
        lesson.groupSubjectId = idOf(
            groupSubjectId, lesson.studentGroup + '\x1f' + lesson.subject);
    }
    table.teacherCount = static_cast<int>(teacherId.size());
    table.groupCount = static_cast<int>(groupId.size());
    table.groupSubjectCount = static_cast<int>(groupSubjectId.size());

    // Same-day period adjacency. (day, period) pairs are assumed unique.
    const int tsCount = table.timeslotCount();
    std::map<std::pair<int, int>, int> byDayPeriod;
    for (int s = 0; s < tsCount; ++s)
    {
        byDayPeriod[{table.timeslots[s].day, table.timeslots[s].period}] = s;
    }
    table.nextSlot.assign(tsCount, -1);
    table.prevSlot.assign(tsCount, -1);
    for (int s = 0; s < tsCount; ++s)
    {
        auto it = byDayPeriod.find(
            {table.timeslots[s].day, table.timeslots[s].period + 1});
        if (it != byDayPeriod.end())
        {
            table.nextSlot[s] = it->second;
            table.prevSlot[it->second] = s;
        }
    }

    // Flatten teacher unavailability. Teachers that appear only in the map but
    // teach no lesson have no interned id and are skipped (they cannot
    // contribute to the score anyway).
    table.teacherUnavailableFlat.assign(
        static_cast<std::size_t>(table.teacherCount) * tsCount, 0);
    for (const auto &[teacher, slots] : table.teacherUnavailable)
    {
        auto it = teacherId.find(teacher);
        if (it == teacherId.end())
            continue;
        for (int s : slots)
        {
            if (s < 0 || s >= tsCount)
                continue;
            table.teacherUnavailableFlat[static_cast<std::size_t>(it->second)
                                             * tsCount
                                         + s]
                = 1;
        }
    }

    table.prepared = true;
}

Score ScoreCalculator::score(Timetable &table)
{
    prepare(table);
    Score score;
    const int tsCount = table.timeslotCount();
    const int roomCount = table.roomCount();

    // Occupancy counters: every pairwise constraint counts C(c, 2) over the
    // lessons sharing a key cell, and both "consecutive periods" constraints
    // are products of adjacent-cell counts.
    std::vector<int> slotRoomCnt(table.slotRoomCount(), 0);
    std::vector<int> teacherSlotCnt(
        static_cast<std::size_t>(table.teacherCount) * tsCount, 0);
    std::vector<int> groupSlotCnt(
        static_cast<std::size_t>(table.groupCount) * tsCount, 0);
    std::vector<int> groupSubjectSlotCnt(
        static_cast<std::size_t>(table.groupSubjectCount) * tsCount, 0);
    std::vector<int> teacherLessons(table.teacherCount, 0);
    std::vector<int> teacherRoomCnt(
        static_cast<std::size_t>(table.teacherCount) * roomCount, 0);

    std::int64_t unavailable = 0;
    for (const Lesson &lesson : table.lessons)
    {
        if (lesson.slotRoom < 0)
            continue;  // unassigned: contributes nothing
        const int ts = table.timeslotIndexOf(lesson.slotRoom);
        const int room = table.roomIndexOf(lesson.slotRoom);
        ++slotRoomCnt[lesson.slotRoom];
        ++teacherSlotCnt[static_cast<std::size_t>(lesson.teacherId) * tsCount
                         + ts];
        ++groupSlotCnt[static_cast<std::size_t>(lesson.groupId) * tsCount + ts];
        ++groupSubjectSlotCnt[static_cast<std::size_t>(lesson.groupSubjectId)
                                  * tsCount
                              + ts];
        ++teacherLessons[lesson.teacherId];
        ++teacherRoomCnt[static_cast<std::size_t>(lesson.teacherId) * roomCount
                         + room];
        if (teacherUnavailableAt(table, lesson.teacherId, ts))
            ++unavailable;  // hard 4: teacherUnavailabilityCost
    }

    // Hard 1-3: conflicts are pair counts per occupancy cell. slotRoom values
    // ARE (timeslot, room) cells, so roomConflictCost falls out of the
    // encoding.
    std::int64_t conflicts = 0;
    for (int c : slotRoomCnt)
        conflicts += pairs(c);  // hard 1: roomConflictCost
    for (int c : teacherSlotCnt)
        conflicts += pairs(c);  // hard 2: teacherConflictCost
    for (int c : groupSlotCnt)
        conflicts += pairs(c);  // hard 3: classConflictCost
    score.hard = -(conflicts + unavailable);

    // Soft 5: teacherRoomChangeCost. Different-room pairs of one teacher =
    // all pairs C(n, 2) minus the same-room pairs sum C(c_r, 2).
    std::int64_t roomChange = 0;
    for (int t = 0; t < table.teacherCount; ++t)
    {
        std::int64_t sameRoomPairs = 0;
        for (int r = 0; r < roomCount; ++r)
        {
            sameRoomPairs += pairs(
                teacherRoomCnt[static_cast<std::size_t>(t) * roomCount + r]);
        }
        roomChange += pairs(teacherLessons[t]) - sameRoomPairs;
    }

    // Soft 6 + 7: consecutive-period pairs = sum over adjacent timeslot cells
    // of count[slot] * count[nextSlot] (each unordered pair counted once).
    // soft 6: teacherTimeEfficiencyCost (a reward)
    std::int64_t adjacentPairs = 0;
    for (int t = 0; t < table.teacherCount; ++t)
    {
        const std::size_t base = static_cast<std::size_t>(t) * tsCount;
        for (int s = 0; s < tsCount; ++s)
        {
            const int next = table.nextSlot[s];
            if (next >= 0)
            {
                adjacentPairs
                    += static_cast<std::int64_t>(teacherSlotCnt[base + s])
                       * teacherSlotCnt[base + next];
            }
        }
    }
    std::int64_t subjectRepeat = 0;  // soft 7: classSubjectRepeatCost
    for (int g = 0; g < table.groupSubjectCount; ++g)
    {
        const std::size_t base = static_cast<std::size_t>(g) * tsCount;
        for (int s = 0; s < tsCount; ++s)
        {
            const int next = table.nextSlot[s];
            if (next >= 0)
            {
                subjectRepeat
                    += static_cast<std::int64_t>(groupSubjectSlotCnt[base + s])
                       * groupSubjectSlotCnt[base + next];
            }
        }
    }
    score.soft
        = static_cast<double>(adjacentPairs - roomChange - subjectRepeat);

    table.score = score;
    return score;
}

}  // namespace st
