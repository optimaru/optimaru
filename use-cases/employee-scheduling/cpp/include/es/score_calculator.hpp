#ifndef ES_SCORE_CALCULATOR_HPP
#define ES_SCORE_CALCULATOR_HPP

#include <set>

#include "es/domain.hpp"

namespace es {

// calculateScore does a full recompute. The per-constraint building blocks are
// exposed as static helpers so the incremental evaluator (IncrementalScore) and
// the full recompute share exactly the same logic -- the incremental score is
// then correct by construction.
class ScoreCalculator {
public:
    static constexpr Minutes MIN_REST_MINUTES = 10 * 60;  // "at least 10 hours"

    static Score calculateScore(EmployeeSchedule& schedule);

    // Interns skills into integer bit ids and precomputes per-shift day indices,
    // filling Employee::skillMask, Shift::requiredSkillBit, Shift::startDay/endDay.
    // Must run before scoring; calculateScore/solve call it, every time, because
    // Python can change a schedule between calls. Throws std::invalid_argument
    // for a shift without 0 <= start < end up to MAX_DAY, for an employeeIndex
    // outside the employee list, or for more than 64 distinct required skills
    // (skills only employees list do not count).
    static void prepare(EmployeeSchedule& schedule);

    // --- shared per-constraint building blocks ---

    // Minutes of a shift on a calendar day. The day's window ends one minute
    // short of midnight (a shift reaching/crossing midnight loses its final
    // minute that day).
    static Minutes minutesOnDay(const Shift& shift, DayIndex day);

    // Sum of a shift's minutes falling on any day in `dates` (only the shift's
    // start day and end day are considered).
    static Minutes overlapWithDates(const Shift& shift, const std::set<DayIndex>& dates);

    // Hard penalty magnitude (>= 0) for a single assigned shift: missing skill
    // (1) + unavailable-date overlap minutes.
    static std::int64_t shiftHardPenalty(const Employee& emp, const Shift& shift);

    // Net soft contribution for a single assigned shift: desired-day reward
    // minus undesired-day penalty (in minutes; may be negative).
    static std::int64_t shiftSoftDelta(const Employee& emp, const Shift& shift);

    // Hard penalty magnitude (>= 0) for a pair of shifts assigned to the SAME
    // employee: overlap minutes + one-shift-per-day + the <10h rest shortfall.
    static std::int64_t pairHardPenalty(const Shift& a, const Shift& b);

    // Load-balancing unfairness (see DefaultLoadBalance.unfairness): rounded to
    // six significant figures, HALF_EVEN.
    static double unfairness(const std::vector<long>& loads);
    // Same, from running aggregates: sum of loads and sum of squared loads.
    static double unfairnessFromAggregates(long sumOfLoads, long sumOfSquares, int itemCount);
};

}  // namespace es

#endif  // ES_SCORE_CALCULATOR_HPP
