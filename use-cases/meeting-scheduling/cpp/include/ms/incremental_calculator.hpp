#ifndef MS_INCREMENTAL_CALCULATOR_HPP
#define MS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ms/domain.hpp"
#include "optimaru/objective.hpp"

namespace ms
{

// The meeting-scheduling implementation of the engine's
// optimaru::Objective — the C++ analog of implementing
// Java's Objective instead of a ConstraintProvider.
//
// All overlap-weighted pair constraints decompose over grain-index units:
// two meetings' overlap == the number of grain offsets they both occupy, so
// each is a sum of C(count, 2) (or count-product) terms over per-offset
// occupancy counters, and moving one assignment only touches its own
// durationInGrains offsets:
//   - hard 1 (roomConflictCost):  per (room, offset) cell counts
//   - hard 3 (requiredAttendanceConflictCost): per (person, offset) required
//     counts; the two-phase insert order (required first, then preferred)
//     also reproduces the Java same-meeting (required, preferred) join
//   - medium 6/7 (requiredAndPreferredAttendanceConflictCost /
//     preferredAttendanceConflictCost): per (person, offset) preferred
//     counts and required x preferred cross products
//   - soft 10 (meetingOverlapCost): per offset counts over all rooms
//   - soft 9 (consecutiveMeetingsCost): start/end counts per offset; a
//     change touches only offsets start-1 and last+1
// The unary constraints (2, 4, 5, 8, 11) are O(1) table lookups, and soft 12
// (roomChangeCost) walks the precomputed shared-attendee neighbors of the one
// changed meeting — O(neighbors), not O(solution).
//
// before_assign retracts the assignment's contribution while
// grainRoom still holds the OLD value; after_assign inserts the NEW
// value. Unassigned assignments (grainRoom == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::score (checked in
// the tests and under ScoreChecks::ScoreChecks::Full).
class MeetingSchedulingIncrementalScoreCalculator final
    : public optimaru::Objective<MeetingSchedule>
{
public:
    void rebuild(MeetingSchedule &solution) override;
    void before_assign(int assignmentIndex) override;
    void after_assign(int assignmentIndex) override;
    Score score() override;

private:
    MeetingSchedule *schedule_ = nullptr;
    int span_ = 0;
    int roomCount_ = 0;
    std::vector<int> assign_;  // grainRoom per assignment (-1 == unassigned)

    // Decoded grainRoom of every assigned assignment (valid where assign_ >=
    // 0), plus flat copies of the per-grain facts and per-meeting durations.
    // Pure caches — they only exist so the roomChangeCost neighbor walk reads
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

    // Running raw totals, one per constraint and named after its registry
    // name (counted positive; signs, the components' kDefaultWeight and the
    // medium fold are applied in score).
    std::int64_t roomConflictCost_ = 0;                            // hard 1
    std::int64_t overtimeCost_ = 0;                                // hard 2
    std::int64_t requiredAttendanceConflictCost_ = 0;              // hard 3
    std::int64_t roomCapacityCost_ = 0;                            // hard 4
    std::int64_t dayCrossingCost_ = 0;                             // hard 5
    std::int64_t requiredAndPreferredAttendanceConflictCost_ = 0;  // medium 6
    std::int64_t preferredAttendanceConflictCost_ = 0;             // medium 7
    std::int64_t meetingEndTimeCost_ = 0;                          // soft 8
    std::int64_t consecutiveMeetingsCost_ = 0;  // soft 9 (x100)
    std::int64_t meetingOverlapCost_ = 0;       // soft 10 (x10)
    std::int64_t largerRoomCapacityCost_ = 0;   // soft 11
    std::int64_t roomChangeCost_ = 0;           // soft 12

    void insert(int assignmentIndex, int grainRoom);
    void retract(int assignmentIndex, int grainRoom);

    // roomChangeCost pair test between the changed assignment (start grain
    // position posA, room roomA, meeting meetingA) and another assigned
    // assignment (posB, roomB, meetingB); returns true when the Java
    // constraint matches. Reads only the flat caches above.
    bool roomChangePairMatches(int posA,
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
