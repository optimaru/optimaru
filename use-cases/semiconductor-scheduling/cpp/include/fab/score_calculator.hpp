#ifndef FAB_SCORE_CALCULATOR_HPP
#define FAB_SCORE_CALCULATOR_HPP

#include <vector>

#include "fab/domain.hpp"
#include "fab/score_terms.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// ScoreCalculator — the monolithic full recompute
// ---------------------------------------------------------------------------
// The straight port of the 0.1.x scorer: rawCostsOf() walks an assignment
// list exactly like the Python `constraint_costs` did (and yields the same
// per-item lists), and the score is the default weighted sum:
//
//   hard = -(sum of the 14 hard terms)                 (weight 1 each)
//   soft = -(preference + min batch size + load imbalance
//            + 10 * weighted tardiness + cycle time + makespan)
//
// The weights are the components' kDefaultWeight; the default plugin
// assembly scores bit-for-bit the same.
class ScoreCalculator
{
public:
    // Validates the schedule (std::invalid_argument, a Python ValueError) and
    // fills its derived lookups: groups, families, setup / transport tables,
    // last option per machine, rows, pins per row, resource users. Idempotent.
    static void prepare(SemiconductorSchedule &schedule);
    // The checks prepare() runs on schedule.assignments, for a new list.
    static void validateAssignments(const SemiconductorSchedule &schedule,
                                    const std::vector<Assignment> &assignments);

    // Data mode: the raw costs of schedule.assignments.
    static RawTotals rawCosts(const SemiconductorSchedule &schedule, RawItems *items = nullptr);
    // Search mode, full recompute: the seed as supplied while every row holds
    // its seed value (and the seed was complete), else decode(rows).
    static RawTotals searchStateRawCosts(const SemiconductorSchedule &schedule);

    // The default score of raw totals.
    static Score scoreOf(const RawTotals &raw);

    // Data mode: scores schedule.assignments and stores score, makespan,
    // totalWeightedTardiness and totalCycleTime (scored = true).
    static Score score(SemiconductorSchedule &schedule);
    // Search mode: the score of the current planning values (full recompute).
    static Score searchStateScore(const SemiconductorSchedule &schedule);

    // makespan / totalWeightedTardiness / totalCycleTime from raw totals.
    static void storeTotals(SemiconductorSchedule &schedule, const RawTotals &raw);
};

}  // namespace fab

#endif  // FAB_SCORE_CALCULATOR_HPP
