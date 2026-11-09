#ifndef ST_CANONICAL_SCORE_CALCULATOR_HPP
#define ST_CANONICAL_SCORE_CALCULATOR_HPP

#include "st/bit_ops.hpp"
#include <cstdint>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"

namespace st::canon
{

struct ObjectiveWeights;  // st/objective_weights.hpp

// Raw per-constraint costs of one assignment (unweighted, positive =
// violation), one member per constraint, named like its registry name. The
// monolithic calculator's full recompute; the default plugin assembly
// reports the same numbers through constraint_costs(). The soft members
// reproduce teacher_scheduling/validate.py's compute_penalties exactly
// (Timetable.penalties). hardTotal() is what the hard score negates (each
// hard member times its component's kDefaultWeight); softPenalty(weights)
// the weighted soft penalty (soft score = -softPenalty). Both are defined in
// canonical_score_calculator.cpp, next to the component constants.
struct Breakdown
{
    // Hard.
    std::int64_t unassignedLessonsCost = 0;  // lessons with slot == -1
    std::int64_t classConflictCost = 0;      // C(n,2) per (class, slot)
    std::int64_t teacherConflictCost = 0;    // C(n,2) per (teacher, slot)
    std::int64_t disallowedSlotCost = 0;     // lessons outside allowedSlot
    std::int64_t roomCapacityCost = 0;       // demand excess per (type, slot)
    std::int64_t courseMaxPeriodsPerDayCost = 0;  // excess per (course, day)
    std::int64_t blockAdjacencyCost = 0;  // missing adjacencies per group
    // Daily (over maxPerDay) plus weekly (over maxPerWeek) excess.
    std::int64_t teacherMaxPeriodsCost = 0;
    std::int64_t teacherMaxConsecutivePeriodsCost = 0;  // run excess
    // Structural: 0 by construction (pinned lessons / merged parallel
    // groups); kept so the breakdown names every default constraint.
    std::int64_t fixedSlotsCost = 0;
    std::int64_t parallelGroupsCost = 0;

    // Soft (raw counts, unweighted).
    std::int64_t teacherGapCost = 0;
    std::int64_t teacherWorkdayCost = 0;
    std::int64_t teacherFreeDayCost = 0;  // worked requested free days
    std::int64_t classGapCost = 0;
    std::int64_t avoidedPeriodCost = 0;
    std::int64_t nonPreferredPeriodCost = 0;

    std::int64_t hardTotal() const;
    double softPenalty(const ObjectiveWeights &weights) const;
    Score toScore(const ObjectiveWeights &weights) const;
};

// Full-recompute scoring for the canonical domain — the reference the
// incremental calculator is checked against (in the tests and under
// ScoreChecks::Full). All mask/pair/block helpers are shared with the incremental
// calculator so the two agree by construction.
//
// The constraints follow benchmarks/school-timetabling/cpsat/teacher_scheduling
// exactly (raw cost per constraint; 0 = no violation):
//   unassignedLessonsCost       one per unassigned lesson (0 once
//                               construction fills all)
//   classConflictCost           C(n,2) per (classGroup, slot) occupancy cell
//   teacherConflictCost         C(n,2) per (teacher, slot); every teacher of
//                               a lesson occupies the slot (team teaching)
//   disallowedSlotCost          one per lesson outside its allowedSlot table
//   roomCapacityCost            per (roomType, slot): max(0, demand -
//                               capacity)
//   courseMaxPeriodsPerDayCost  per (courseKey, day): max(0, lessons -
//                               courseDailyLimit)
//   blockAdjacencyCost          per block group with k assigned members:
//                               (k-1) minus the same-day consecutive-period
//                               adjacencies after sorting by (day, period),
//                               plus one per missing member when
//                               0 < k < groupSize — 0 iff all members form
//                               one run on one day, or none is assigned
//   teacherMaxPeriodsCost       per (teacher, day): max(0, distinct busy
//                               periods - maxPerDay); per teacher: max(0,
//                               weekly distinct busy periods - maxPerWeek)
//   teacherMaxConsecutivePeriodsCost  per (teacher, day): sum over busy runs
//                               of max(0, run - maxConsecutive)
//   fixedSlotsCost, parallelGroupsCost  0 by construction (pinned lessons,
//                               merged parallel groups), never scored
//   teacherGapCost / classGapCost  per (teacher|class, day) with any lesson:
//                               last - first + 1 - count over the DISTINCT
//                               busy periods (an unavailable period wedged
//                               between lessons counts as a gap, exactly
//                               like validate.py)
//   teacherWorkdayCost          number of (teacher, day) pairs with any
//                               lesson
//   teacherFreeDayCost          the same, restricted to the teacher's
//                               preferred free days (worked requested free
//                               days)
//   avoidedPeriodCost           per assigned preference unit whose period is
//                               in the avoided mask
//   nonPreferredPeriodCost      per assigned preference unit off a non-empty
//                               preferred mask
class ScoreCalculator
{
public:
    // Full recompute of the score (also stored in table.score): hard at the
    // components' kDefaultWeight, soft at `weights` (default: the soft
    // components' kDefaultWeight). The model carries no weights.
    static Score score(Timetable &table);
    static Score score(Timetable &table,
                                const ObjectiveWeights &weights);

    // Full per-constraint recompute (used by tests and score).
    static Breakdown computeBreakdown(Timetable &table);

    // Normalizes the instance in place: derives teacher/class/course/roomType
    // counts, sizes every per-id vector with the CP-SAT defaults, builds the
    // block-group member lists and the (day, period) -> slot table. Idempotent
    // (guarded by Timetable::prepared). Throws std::invalid_argument on
    // malformed input. Must run before scoring; score/solve call it.
    static void prepare(Timetable &table);

    // --- shared building blocks (also used by the incremental calculator) ---

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

    static std::int64_t max0(std::int64_t v) { return v > 0 ? v : 0; }

    // teacherGapCost / classGapCost of one day's busy-period mask:
    // last - first + 1 - count (0 for an empty mask).
    static int gapOf(unsigned mask)
    {
        if (mask == 0)
            return 0;
        const int hi = 31 - st::bits::clz(mask);
        const int lo = st::bits::ctz(mask);
        return hi - lo + 1 - st::bits::popcount(mask);
    }

    // teacherMaxConsecutivePeriodsCost of one day's busy-period mask: sum
    // over maximal 1-runs of max(0, runLength - limit).
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

    // blockAdjacencyCost of one block group: (k-1) minus the same-day
    // consecutive adjacencies among the k assigned members' (day, period)
    // pairs (sorted in place), plus one point per member the group is
    // missing.
    //
    // The missing-member charge is what keeps a HALF-placed block from being
    // free: adjacency alone scores a lone survivor at 0 (k=1 needs no
    // neighbour) while the CP-SAT reference calls the same timetable "no
    // 2-period block". `groupSize` is the group's full member count; pass
    // dayPeriods.size() when the caller knows every member is assigned.
    //
    // A group with NOTHING assigned stays at 0 on purpose — that is the one
    // shape partial solving may leave behind, and unassignedLessonsCost
    // charges for it there.
    static std::int64_t
    blockViolation(std::vector<std::pair<int, int>> &dayPeriods,
                   std::size_t groupSize);
};

}  // namespace st::canon

#endif  // ST_CANONICAL_SCORE_CALCULATOR_HPP
