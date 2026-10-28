#ifndef MS_INCREMENTAL_CALCULATOR_HPP
#define MS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ms/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace ms
{

// The meeting-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// All overlap-weighted pair constraints decompose over grain-index units:
// two meetings' overlap == the number of grain offsets they both occupy, so
// each is a sum of C(count, 2) (or count-product) terms over per-offset
// occupancy counters, and moving one assignment only touches its own
// durationInGrains offsets:
//   - hard 1 (roomConflict):      per (room, offset) cell counts
//   - hard 3 (requiredAttendanceConflict):   per (person, offset) required
//     counts; the two-phase insert order (required first, then preferred)
//     also reproduces the Java same-meeting (required, preferred) join
//   - medium 6/7 (required+preferred / preferred conflicts): per (person,
//     offset) preferred counts and required x preferred cross products
//   - soft 10 (overlappingMeetings): per offset counts over all rooms
//   - soft 9 (oneBreakBetweenConsecutiveMeetings): start/end counts per
//     offset; a change touches only offsets start-1 and last+1
// The unary constraints (2, 4, 5, 8, 11) are O(1) table lookups, and soft 12
// (roomStability) walks the precomputed shared-attendee neighbors of the one
// changed meeting — O(neighbors), not O(solution).
//
// beforeVariableChanged retracts the assignment's contribution while
// grainRoom still holds the OLD value; afterVariableChanged inserts the NEW
// value. Unassigned assignments (grainRoom == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in
// the tests and under EnvironmentMode::FULL_ASSERT).
class MeetingSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<MeetingSchedule>
{
public:
    void resetWorkingSolution(MeetingSchedule &workingSolution) override;
    void beforeVariableChanged(int assignmentIndex) override;
    void afterVariableChanged(int assignmentIndex) override;
    Score calculateScore() override;

private:
    MeetingSchedule *schedule_ = nullptr;
    int span_ = 0;
    int roomCount_ = 0;
    std::vector<int> assign_;  // grainRoom per assignment (-1 == unassigned)

    // Decoded grainRoom of every assigned assignment (valid where assign_ >=
    // 0), plus flat copies of the per-grain facts and per-meeting durations.
    // Pure caches — they only exist so the room-stability neighbor walk reads
    // flat int arrays instead of dividing grainRooms and gathering through
    // TimeGrain / Meeting structs on every pair test (the delta path is >90%
    // of solve time; see the tuning history in solver.hpp).
    std::vector<int> posOf_;            // per assignment: timeGrainPos
    std::vector<int> roomOf_;           // per assignment: room index
    std::vector<int> dayOfPos_;         // per grain pos: dayOfYear
    std::vector<int> minuteOfPos_;      // per grain pos: startingMinuteOfDay
    std::vector<int> grainIndexOfPos_;  // per grain pos: grainIndex
    std::vector<int> durationOf_;       // per meeting: durationInGrains

    // Occupancy counters over grain-index offsets.
    std::vector<int> roomGrainCnt_;   // room x offset
    std::vector<int> personReqCnt_;   // person x offset (required)
    std::vector<int> personPrefCnt_;  // person x offset (preferred)
    std::vector<int> anyGrainCnt_;    // offset (all rooms)
    std::vector<int> startCnt_;       // assignments starting at offset
    std::vector<int> endCnt_;         // assignments ending at offset

    // Running totals (counted positive; signs/weights in calculateScore).
    std::int64_t roomConflict_ = 0;  // hard 1
    std::int64_t overtime_ = 0;      // hard 2
    std::int64_t reqConflict_ = 0;   // hard 3
    std::int64_t capacity_ = 0;      // hard 4
    std::int64_t sameDay_ = 0;       // hard 5
    std::int64_t reqPref_ = 0;       // medium 6
    std::int64_t prefConflict_ = 0;  // medium 7
    std::int64_t asap_ = 0;          // soft 8
    std::int64_t breaks_ = 0;        // soft 9 (x100)
    std::int64_t overlapping_ = 0;   // soft 10 (x10)
    std::int64_t largerRooms_ = 0;   // soft 11
    std::int64_t stability_ = 0;     // soft 12

    void insert(int assignmentIndex, int grainRoom);
    void retract(int assignmentIndex, int grainRoom);

    // Room-stability pair test between the changed assignment (start grain
    // position posA, room roomA, meeting meetingA) and another assigned
    // assignment (posB, roomB, meetingB); returns true when the Java
    // constraint matches. Reads only the flat caches above.
    bool stabilityPairMatches(int posA,
                              int roomA,
                              int meetingA,
                              int posB,
                              int roomB,
                              int meetingB) const;

    std::size_t roomGrainOffset(int room, int offset) const
    {
        return static_cast<std::size_t>(room) * span_ + offset;
    }
    std::size_t personOffset(int person, int offset) const
    {
        return static_cast<std::size_t>(person) * span_ + offset;
    }
};

}  // namespace ms

#endif  // MS_INCREMENTAL_CALCULATOR_HPP
