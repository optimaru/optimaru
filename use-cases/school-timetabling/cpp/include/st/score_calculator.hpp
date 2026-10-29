#ifndef ST_SCORE_CALCULATOR_HPP
#define ST_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "st/domain.hpp"

namespace st
{

// Port of the reference school-timetabling TimeTableConstraintProvider, plus
// one extension (teacherUnavailable).
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore):
//   hard 1. roomConflict            -1 per lesson pair in the same timeslot
//                                      and room
//   hard 2. teacherConflict         -1 per same-teacher lesson pair in the
//                                      same timeslot
//   hard 3. studentGroupConflict    -1 per same-group lesson pair in the same
//                                      timeslot
//   hard 4. teacherUnavailable      -1 per lesson in a timeslot its teacher
//                                      is unavailable (extension; zero when
//                                      the map is empty)
//   soft 5. teacherRoomStability    -1 per same-teacher lesson pair in
//                                      different rooms
//   soft 6. teacherTimeEfficiency   +1 per same-teacher lesson pair on the
//                                      same day in consecutive periods
//   soft 7. studentGroupSubjectVariety  -1 per same-group same-subject lesson
//                                      pair on the same day in consecutive
//                                      periods
// Unassigned lessons (slotRoom == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score calculateScore(Timetable &table);

    // Interns teachers, student groups and (group, subject) pairs into dense
    // integer ids, builds the same-day period adjacency (nextSlot/prevSlot)
    // and flattens teacherUnavailable for O(1) lookup. Idempotent (guarded by
    // Timetable::prepared). Must run before scoring; calculateScore/solve
    // call it.
    static void prepare(Timetable &table);

    // --- shared building blocks ---

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

    static bool teacherUnavailableAt(const Timetable &table,
                                     int teacherId,
                                     int timeslotIndex)
    {
        return table.teacherUnavailableFlat[static_cast<std::size_t>(teacherId)
                                                * table.timeslotCount()
                                            + timeslotIndex]
               != 0;
    }
};

}  // namespace st

#endif  // ST_SCORE_CALCULATOR_HPP
