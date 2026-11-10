#ifndef ST_SCORE_CALCULATOR_HPP
#define ST_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "st/domain.hpp"

namespace st
{

// The quickstart model's score as one monolithic function: six pairwise
// terms plus teacherUnavailabilityCost. The quickstart model has no
// constraint registry (solve(mode="quickstart") takes no constraints=); the
// terms carry the same *Cost names as the canonical model where they mean the
// same thing (teacherConflictCost, classConflictCost).
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (Score):
//   hard 1. roomConflictCost          -1 per lesson pair in the same
//                                        timeslot and room
//   hard 2. teacherConflictCost       -1 per same-teacher lesson pair in the
//                                        same timeslot
//   hard 3. classConflictCost         -1 per same-group (class) lesson pair
//                                        in the same timeslot
//   hard 4. teacherUnavailabilityCost -1 per lesson in a timeslot its
//                                        teacher is unavailable (zero when
//                                        the map is empty)
//   soft 5. teacherRoomChangeCost     -1 per same-teacher lesson pair in
//                                        different rooms
//   soft 6. teacherTimeEfficiencyCost +1 per same-teacher lesson pair on the
//                                        same day in consecutive periods (a
//                                        REWARD: raw cost <= 0)
//   soft 7. classSubjectRepeatCost    -1 per same-group same-subject lesson
//                                        pair on the same day in consecutive
//                                        periods
// Unassigned lessons (slotRoom == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score score(Timetable &table);

    // Interns teachers, student groups and (group, subject) pairs into dense
    // integer ids, builds the same-day period adjacency (nextSlot/prevSlot)
    // and flattens teacherUnavailable for O(1) lookup. Idempotent (guarded by
    // Timetable::prepared). Must run before scoring; score/solve
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
