#ifndef MS_SCORE_CALCULATOR_HPP
#define MS_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "ms/domain.hpp"

namespace ms
{

// Per-constraint violation totals (raw, unweighted, counted positive) — the
// exact quantities the Java MeetingSchedulingConstraintProvider penalizes.
// Used by the tests (hand-computed per-constraint cases like the Java
// ConstraintVerifier tests) and by the demo output.
struct Breakdown
{
    // Hard (weight 1 each).
    std::int64_t roomConflictGrains = 0;      // overlap grains per room pair
    std::int64_t overtimeSum = 0;             // sum of lastTimeGrainIndex
    std::int64_t requiredConflictGrains = 0;  // shared-required overlap grains
    std::int64_t roomCapacityShortage = 0;    // requiredCapacity - capacity
    std::int64_t startEndDayViolations = 0;   // meetings crossing a day border

    // Medium (weight 1 each, folded into soft via kMediumScale).
    std::int64_t requiredPreferredConflictGrains = 0;
    std::int64_t preferredConflictGrains = 0;

    // Soft.
    std::int64_t lastGrainIndexSum = 0;         // x1   "as soon as possible"
    std::int64_t consecutiveMeetingPairs = 0;   // x100 back-to-back, no break
    std::int64_t overlappingMeetingGrains = 0;  // x10  any-meeting overlap
    std::int64_t largerRoomCapacitySum = 0;     // x1   larger rooms exist
    std::int64_t roomStabilityViolations = 0;   // x1   attendee room switch

    std::int64_t hardTotal() const
    {
        return roomConflictGrains + overtimeSum + requiredConflictGrains
               + roomCapacityShortage + startEndDayViolations;
    }
    std::int64_t mediumTotal() const
    {
        return requiredPreferredConflictGrains + preferredConflictGrains;
    }
    std::int64_t softTotal() const
    {
        return lastGrainIndexSum + 100 * consecutiveMeetingPairs
               + 10 * overlappingMeetingGrains + largerRoomCapacitySum
               + roomStabilityViolations;
    }
};

// Port of the reference meeting-scheduling MeetingSchedulingConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (Java level in brackets; weights per unit):
//   [hard]   1. roomConflict          -1 per overlap grain of two meetings in
//                                        the same room
//   [hard]   2. avoidOvertime         -lastTimeGrainIndex when no TimeGrain
//                                        exists at the meeting's last index
//   [hard]   3. requiredAttendanceConflict  -1 per overlap grain per shared
//                                        required attendee pair
//   [hard]   4. requiredRoomCapacity  -(requiredCapacity - capacity) when the
//                                        room is too small
//   [hard]   5. startAndEndOnSameDay  -1 when the last grain exists on a
//                                        different day than the first
//   [medium] 6. requiredAndPreferredAttendanceConflict  -1 per overlap grain
//                                        per (required, preferred) pair
//   [medium] 7. preferredAttendanceConflict  -1 per overlap grain per shared
//                                        preferred attendee pair
//   [soft]   8. doMeetingsAsSoonAsPossible  -lastTimeGrainIndex per meeting
//   [soft]   9. oneBreakBetweenConsecutiveMeetings  -100 per meeting pair
//                                        scheduled back-to-back (no break)
//   [soft]  10. overlappingMeetings   -10 per overlap grain of any two
//                                        different meetings
//   [soft]  11. assignLargerRoomsFirst  -(capacity(R) - capacity(room)) per
//                                        strictly larger room R
//   [soft]  12. roomStability         -1 per shared attendee whose next
//                                        meeting (gap <= 2 grains) switches
//                                        rooms
// Unassigned assignments (grainRoom == -1) contribute nothing: the
// construction heuristic scores partially initialized solutions. (Java's
// forEachIncludingUnassigned variants score some half-assigned states — e.g.
// requiredRoomCapacity with room == null — but with the composite encoding
// both Java variables are assigned or neither, and on fully assigned
// solutions the semantics match exactly.)
class ScoreCalculator
{
public:
    static Score calculateScore(MeetingSchedule &schedule);

    // --- shared building blocks (also used by the plugin components) ---

    // Java MeetingAssignment::calculateOverlap: overlap of two grain-index
    // intervals [start, end) (start inclusive, end exclusive).
    static std::int64_t
    overlapGrains(int start1, int end1, int start2, int end2)
    {
        const int lo = start1 > start2 ? start1 : start2;
        const int hi = end1 < end2 ? end1 : end2;
        return hi > lo ? hi - lo : 0;
    }

    // Java TimeGrain::compareTo: by dayOfYear, then startingMinuteOfDay.
    static int compareTimeGrains(const TimeGrain &a, const TimeGrain &b)
    {
        if (a.dayOfYear != b.dayOfYear)
            return a.dayOfYear < b.dayOfYear ? -1 : 1;
        if (a.startingMinuteOfDay != b.startingMinuteOfDay)
            return a.startingMinuteOfDay < b.startingMinuteOfDay ? -1 : 1;
        return 0;
    }

    // Full per-constraint recompute (runs prepare() first).
    static Breakdown computeBreakdown(MeetingSchedule &schedule);

    // Assembles the engine score from a breakdown (the medium -> soft folding
    // in one place).
    static Score toScore(const Breakdown &breakdown)
    {
        Score score;
        score.hard = -breakdown.hardTotal();
        score.soft
            = -(static_cast<double>(breakdown.mediumTotal()) * kMediumScale
                + static_cast<double>(breakdown.softTotal()));
        return score;
    }

    // Builds the derived tables on MeetingSchedule (grain occupancy span,
    // larger-room penalties, attendance conflict arcs, 1:1 meeting <->
    // assignment map). Idempotent (guarded by MeetingSchedule::prepared).
    // Must run before scoring; calculateScore/solve call it. Throws
    // std::invalid_argument on a malformed instance (bad indices, duration
    // < 1, or two assignments sharing one meeting).
    static void prepare(MeetingSchedule &schedule);
};

}  // namespace ms

#endif  // MS_SCORE_CALCULATOR_HPP
