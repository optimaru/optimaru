#ifndef ST_INCREMENTAL_CALCULATOR_HPP
#define ST_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "st/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace st
{

// The school-timetabling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is a function of occupancy counts, so a single variable
// change is O(1):
//   - hard 1-3 (room/teacher/group conflict): C(c, 2) pair sums per occupancy
//     cell. Moving one lesson in/out of a cell with c other members changes
//     the pair count by exactly c.
//   - hard 4 (teacherUnavailable): one flat-table lookup per change.
//   - soft 5 (teacherRoomStability): different-room pairs per teacher =
//     C(n, 2) - sum_r C(c_r, 2), maintained from the per-teacher lesson count
//     n and per-room counts c_r (insert delta: n - c_room, counts before the
//     insert; retract mirrors it).
//   - soft 6/7 (consecutive periods): adjacent-cell count products; a change
//     touches only the prevSlot/nextSlot cells of its (key, timeslot).
//
// beforeVariableChanged retracts the lesson's contribution while slotRoom
// still holds the OLD value; afterVariableChanged inserts the NEW value.
// Unassigned lessons (slotRoom == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions. The full-recompute
// reference is ScoreCalculator::calculateScore (checked in the tests and
// under EnvironmentMode::FULL_ASSERT).
class SchoolTimetablingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<Timetable>
{
public:
    void resetWorkingSolution(Timetable &workingSolution) override;
    void beforeVariableChanged(int lessonIndex) override;
    void afterVariableChanged(int lessonIndex) override;
    Score calculateScore() override;

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

    // Running totals (all counted positive; signs applied in calculateScore).
    std::int64_t conflictPairs_ = 0;  // hard 1 + 2 + 3
    std::int64_t unavailable_ = 0;    // hard 4
    std::int64_t stability_ = 0;      // soft 5 (penalty)
    std::int64_t efficiency_ = 0;     // soft 6 (reward)
    std::int64_t variety_ = 0;        // soft 7 (penalty)

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
