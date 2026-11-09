#ifndef MS_SCORE_CALCULATOR_HPP
#define MS_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "ms/domain.hpp"

namespace ms
{

// Per-constraint violation totals (raw, unweighted, counted positive) — the
// exact quantities the Java MeetingSchedulingConstraintProvider penalizes.
// Each member is named after its constraint's registry name
// (ms/constraints/<subject>_cost.hpp). Used by the monolithic score, the C++
// tests (hand-computed per-constraint cases like the Java ConstraintVerifier
// tests) and the demo output; Python reads the same per-constraint values
// through constraint_costs().
struct Breakdown
{
    // Hard.
    std::int64_t roomConflictCost = 0;  // overlap grains per room pair
    std::int64_t overtimeCost = 0;      // sum of lastTimeGrainIndex
    std::int64_t requiredAttendanceConflictCost = 0;  // shared-required grains
    std::int64_t roomCapacityCost = 0;  // requiredCapacity - capacity
    std::int64_t dayCrossingCost = 0;   // meetings crossing a day border

    // Medium (folded into soft via kMediumFold).
    std::int64_t requiredAndPreferredAttendanceConflictCost = 0;
    std::int64_t preferredAttendanceConflictCost = 0;

    // Soft.
    std::int64_t meetingEndTimeCost = 0;       // sum of lastTimeGrainIndex
    std::int64_t consecutiveMeetingsCost = 0;  // back-to-back pairs, no break
    std::int64_t meetingOverlapCost = 0;       // any-meeting overlap grains
    std::int64_t largerRoomCapacityCost = 0;   // capacity of larger rooms
    std::int64_t roomChangeCost = 0;           // attendee room switches

    // Weighted totals per level at the components' kDefaultWeight (all
    // integral, so the sums are exact int64; score_calculator.cpp).
    std::int64_t hardTotal() const;
    std::int64_t mediumTotal() const;
    std::int64_t softTotal() const;
};

// Port of the reference meeting-scheduling MeetingSchedulingConstraintProvider.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (level in brackets; default weights per unit, the components'
// kDefaultWeight):
//   [hard]   1. roomConflictCost      -1 per overlap grain of two meetings in
//                                        the same room
//   [hard]   2. overtimeCost          -lastTimeGrainIndex when no TimeGrain
//                                        exists at the meeting's last index
//   [hard]   3. requiredAttendanceConflictCost  -1 per overlap grain per
//                                        shared required attendee pair
//   [hard]   4. roomCapacityCost      -(requiredCapacity - capacity) when the
//                                        room is too small
//   [hard]   5. dayCrossingCost       -1 when the last grain exists on a
//                                        different day than the first
//   [medium] 6. requiredAndPreferredAttendanceConflictCost  -1 per overlap
//                                        grain per (required, preferred) pair
//   [medium] 7. preferredAttendanceConflictCost  -1 per overlap grain per
//                                        shared preferred attendee pair
//   [soft]   8. meetingEndTimeCost    -lastTimeGrainIndex per meeting
//   [soft]   9. consecutiveMeetingsCost  -100 per meeting pair scheduled
//                                        back-to-back (no break)
//   [soft]  10. meetingOverlapCost    -10 per overlap grain of any two
//                                        different meetings
//   [soft]  11. largerRoomCapacityCost  -(capacity(R) - capacity(room)) per
//                                        strictly larger room R
//   [soft]  12. roomChangeCost        -1 per shared attendee whose next
//                                        meeting (gap <= 2 grains) switches
//                                        rooms
// Unassigned assignments (grainRoom == -1) contribute nothing: the
// construction heuristic scores partially initialized solutions. (Java's
// forEachIncludingUnassigned variants score some half-assigned states — e.g.
// roomCapacityCost with room == null — but with the composite encoding
// both Java variables are assigned or neither, and on fully assigned
// solutions the semantics match exactly.)
class ScoreCalculator
{
public:
    static Score score(MeetingSchedule &schedule);

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

    // Assembles the engine score from a breakdown, folding medium into soft
    // with the domain's kMediumFold (engine soft = soft + medium * 1e6,
    // exact for these integral totals: the old
    // -(medium * kMediumScale + soft)).
    static Score toScore(const Breakdown &breakdown)
    {
        return kMediumFold.to_engine(
            -breakdown.hardTotal(),
            -static_cast<double>(breakdown.mediumTotal()),
            -static_cast<double>(breakdown.softTotal()));
    }

    // Builds the derived tables on MeetingSchedule (grain occupancy span,
    // larger-room penalties, attendance conflict arcs, 1:1 meeting <->
    // assignment map). Idempotent (guarded by MeetingSchedule::prepared).
    // Must run before scoring; score/solve call it. Throws
    // std::invalid_argument on a malformed instance (bad indices, duration
    // < 1, or two assignments sharing one meeting).
    static void prepare(MeetingSchedule &schedule);
};

}  // namespace ms

#endif  // MS_SCORE_CALCULATOR_HPP
