#ifndef FP_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP
#define FP_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Soft (default, weight 1): minimizeMakespan — the SQUARE of how long each
// line stays busy, which is what balances work across lines instead of merely
// shortening the total. An empty line costs nothing.
//
// The squaring lives in cost() rather than in `exponent` on purpose: the
// exponent shapes the WEIGHTED cost of a unit, and here the square is the
// constraint's own definition, so a caller can still shape it further.
struct MinimizeMakespan final : FpUnitConstraint
{
    explicit MinimizeMakespan(double weight = 1.0)
        : FpUnitConstraint("minimizeMakespan", weight, /*hard=*/false)
    {
    }
    void onReset(const PackagingSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const PackagingSchedule &schedule,
                int unit,
                const std::vector<int> & /*members*/) const override
    {
        if (detail::isPool(schedule, unit))
            return 0.0;
        const std::vector<int> &jobs = detail::jobsOfUnit(schedule, unit);
        if (jobs.empty())
            return 0.0;
        const Minutes end = detail::walkChain(
            schedule, unit, jobs, [](int, Minutes, Minutes, Minutes) {});
        const Minutes makespan = end - schedule.lines[unit].startDateTime;
        return static_cast<double>(makespan * makespan);
    }
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP
