#ifndef ST_CANONICAL_SCORE_CALCULATOR_HPP
#define ST_CANONICAL_SCORE_CALCULATOR_HPP

#include "st/bit_ops.hpp"
#include <cstdint>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"

namespace st::canon
{

// Per-constraint violation counts of one assignment. hardTotal() is the
// number the hard score negates; softPenalty(weights) the weighted soft
// penalty (soft score = -softPenalty). The soft fields reproduce
// teacher_scheduling/validate.py's compute_penalties exactly, key for key.
struct Breakdown
{
    // Hard (H2 is enforced by pinning, not counted here).
    std::int64_t unassigned = 0;           // H1: lessons with slot == -1
    std::int64_t classConflicts = 0;       // H3: C(n,2) per (class, slot)
    std::int64_t teacherConflicts = 0;     // H4: C(n,2) per (teacher, slot)
    std::int64_t slotNotAllowed = 0;       // H5: lesson outside allowedSlot
    std::int64_t roomOverCapacity = 0;     // H6: demand excess per (type, slot)
    std::int64_t courseDailyExcess = 0;    // H7: count excess per (course, day)
    std::int64_t blockBreaks = 0;          // H8: missing adjacencies per group
    std::int64_t teacherDailyExcess = 0;   // H9: over maxPerDay
    std::int64_t teacherWeeklyExcess = 0;  // H9: over maxPerWeek
    std::int64_t consecutiveExcess = 0;    // H10: run length over the limit

    // Soft (raw counts, unweighted).
    std::int64_t teacherGap = 0;          // S1
    std::int64_t teacherWorkday = 0;      // S2
    std::int64_t teacherFreeDay = 0;      // S3
    std::int64_t classGap = 0;            // S4
    std::int64_t avoidedPeriod = 0;       // S5
    std::int64_t nonPreferredPeriod = 0;  // S6

    std::int64_t hardTotal() const
    {
        return unassigned + classConflicts + teacherConflicts + slotNotAllowed
               + roomOverCapacity + courseDailyExcess + blockBreaks
               + teacherDailyExcess + teacherWeeklyExcess + consecutiveExcess;
    }
    std::int64_t softPenalty(const ObjectiveWeights &w) const
    {
        return w.teacherGap * teacherGap + w.teacherWorkday * teacherWorkday
               + w.teacherFreeDay * teacherFreeDay + w.classGap * classGap
               + w.avoidedPeriod * avoidedPeriod
               + w.nonPreferredPeriod * nonPreferredPeriod;
    }
    Score toScore(const ObjectiveWeights &w) const
    {
        return {-hardTotal(), -static_cast<double>(softPenalty(w))};
    }
};

// Full-recompute scoring for the canonical domain — the reference the
// incremental calculator is checked against (in the tests and under
// FULL_ASSERT). All mask/pair/block helpers are shared with the incremental
// calculator so the two agree by construction.
//
// H1~H10 / S1~S6 follow benchmarks/school-timetabling/cpsat/teacher_scheduling exactly:
//   H1  one hard point per unassigned lesson (0 once construction fills all)
//   H2  fixed slots — enforced by Lesson::pinned, never scored
//   H3  class conflict: C(n,2) per (classGroup, slot) occupancy cell
//   H4  teacher conflict: C(n,2) per (teacher, slot); every teacher of a
//       lesson occupies the slot (team teaching)
//   H5  one hard point per lesson assigned outside its allowedSlot table
//   H6  per (roomType, slot): max(0, demand - capacity)
//   H7  per (courseKey, day): max(0, lessons - courseDailyLimit)
//   H8  per block group with k assigned members: (k-1) minus the number of
//       same-day consecutive-period adjacencies after sorting by (day,
//       period), plus one per missing member when 0 < k < groupSize — 0 iff
//       all members form one run on one day, or none of them is assigned
//   H9  per (teacher, day): max(0, distinct busy periods - maxPerDay); per
//       teacher: max(0, weekly distinct busy periods - maxPerWeek)
//   H10 per (teacher, day): sum over busy runs of max(0, run - maxConsecutive)
//   S1/S4  per (teacher|class, day) with any lesson: last - first + 1 - count
//          over the DISTINCT busy periods (an unavailable period wedged
//          between lessons counts as a gap, exactly like validate.py)
//   S2  number of (teacher, day) pairs with any lesson
//   S3  S2 restricted to the teacher's preferred free days
//   S5  per assigned lesson whose period is in the course's avoided mask
//   S6  per assigned lesson off a non-empty preferred mask
class ScoreCalculator
{
public:
    static Score calculateScore(Timetable &table);

    // Full per-constraint recompute (used by tests and calculateScore).
    static Breakdown computeBreakdown(Timetable &table);

    // Normalizes the instance in place: derives teacher/class/course/roomType
    // counts, sizes every per-id vector with the CP-SAT defaults, builds the
    // block-group member lists and the (day, period) -> slot table. Idempotent
    // (guarded by Timetable::prepared). Throws std::invalid_argument on
    // malformed input. Must run before scoring; calculateScore/solve call it.
    static void prepare(Timetable &table);

    // --- shared building blocks (also used by the incremental calculator) ---

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

    static std::int64_t max0(std::int64_t v) { return v > 0 ? v : 0; }

    // S1/S4 gap of one day's busy-period mask: last - first + 1 - count
    // (0 for an empty mask).
    static int gapOf(unsigned mask)
    {
        if (mask == 0)
            return 0;
        const int hi = 31 - st::bits::clz(mask);
        const int lo = st::bits::ctz(mask);
        return hi - lo + 1 - st::bits::popcount(mask);
    }

    // H10 excess of one day's busy-period mask: sum over maximal 1-runs of
    // max(0, runLength - limit).
    static std::int64_t runExcessOf(unsigned mask, int limit)
    {
        std::int64_t excess = 0;
        while (mask != 0)
        {
            mask >>= st::bits::ctz(mask);          // skip to the next run
            const int run = st::bits::ctz(~mask);  // its length
            if (run > limit)
                excess += run - limit;
            mask >>= run;
        }
        return excess;
    }

    // H8 violation of one block group: (k-1) minus the same-day consecutive
    // adjacencies among the k assigned members' (day, period) pairs (sorted
    // in place), plus one point per member the group is missing.
    //
    // The missing-member charge is what keeps a HALF-placed block from being
    // free: adjacency alone scores a lone survivor at 0 (k=1 needs no
    // neighbour) while the CP-SAT reference calls the same timetable "no 2-period
    // block". `groupSize` is the group's full member count; pass
    // dayPeriods.size() when the caller knows every member is assigned.
    //
    // A group with NOTHING assigned stays at 0 on purpose — that is the one
    // shape partial solving may leave behind, and H1 charges for it there.
    static std::int64_t
    blockViolation(std::vector<std::pair<int, int>> &dayPeriods,
                   std::size_t groupSize);
};

}  // namespace st::canon

#endif  // ST_CANONICAL_SCORE_CALCULATOR_HPP
