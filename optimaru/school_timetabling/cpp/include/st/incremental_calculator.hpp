#ifndef ST_INCREMENTAL_CALCULATOR_HPP
#define ST_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "st/domain.hpp"
#include "optimaru/score/objective.hpp"

namespace st
{

// The school-timetabling implementation of the engine's
// optimaru::Objective: one hand-written incremental calculator for all
// seven terms.
//
// Every constraint is a function of occupancy counts, so a single variable
// change is O(1):
//   - hard 1-3 (roomConflictCost / teacherConflictCost / classConflictCost):
//     C(c, 2) pair sums per occupancy cell. Moving one lesson in/out of a
//     cell with c other members changes the pair count by exactly c.
//   - hard 4 (teacherUnavailabilityCost): one flat-table lookup per change.
//   - soft 5 (teacherRoomChangeCost): different-room pairs per teacher =
//     C(n, 2) - sum_r C(c_r, 2), maintained from the per-teacher lesson count
//     n and per-room counts c_r (insert delta: n - c_room, counts before the
//     insert; retract mirrors it).
//   - soft 6/7 (teacherTimeEfficiencyCost / classSubjectRepeatCost,
//     consecutive periods): adjacent-cell count products; a change
//     touches only the prevSlot/nextSlot cells of its (key, timeslot).
//
// before_assign retracts the lesson's contribution while slotRoom
// still holds the OLD value; after_assign inserts the NEW value.
// Unassigned lessons (slotRoom == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions. The full-recompute
// reference is ScoreCalculator::score (checked in the tests and
// under ScoreChecks::ScoreChecks::Full).
class SchoolTimetablingIncrementalScoreCalculator final
    : public optimaru::Objective<Timetable>
{
public:
    void rebuild(Timetable &solution) override;
    void before_assign(int lessonIndex) override;
    void after_assign(int lessonIndex) override;
    Score score() override;

private:
    Timetable *table_ = nullptr;
    int tsCount_ = 0;
    int roomCount_ = 0;
    std::vector<int> assign_;  // slotRoom per lesson (-1 == unassigned)

    // Occupancy counters (same shapes as the full recompute's locals).
    std::vector<int> slotRoomCnt_;
    std::vector<int> teacherSlotCnt_;
    std::vector<int> groupSlotCnt_;
    std::vector<int> groupSubjectSlotCnt_;
    std::vector<int> teacherLessons_;
    std::vector<int> teacherRoomCnt_;

    // Running totals (all counted positive; signs applied in score).
    std::int64_t conflictPairs_ = 0;  // hard 1 + 2 + 3
    std::int64_t unavailable_ = 0;    // hard 4: teacherUnavailabilityCost
    std::int64_t roomChange_ = 0;     // soft 5: teacherRoomChangeCost
    std::int64_t adjacentPairs_ = 0;  // soft 6: teacherTimeEfficiencyCost
    std::int64_t subjectRepeat_ = 0;  // soft 7: classSubjectRepeatCost

    void insert(int lessonIndex, int slotRoom);
    void retract(int lessonIndex, int slotRoom);

    std::size_t teacherSlotOffset(int teacherId, int ts) const
    {
        return static_cast<std::size_t>(teacherId) * tsCount_ + ts;
    }
    std::size_t groupSlotOffset(int groupId, int ts) const
    {
        return static_cast<std::size_t>(groupId) * tsCount_ + ts;
    }
    std::size_t groupSubjectSlotOffset(int groupSubjectId, int ts) const
    {
        return static_cast<std::size_t>(groupSubjectId) * tsCount_ + ts;
    }
    std::size_t teacherRoomOffset(int teacherId, int room) const
    {
        return static_cast<std::size_t>(teacherId) * roomCount_ + room;
    }
};

}  // namespace st

#endif  // ST_INCREMENTAL_CALCULATOR_HPP
