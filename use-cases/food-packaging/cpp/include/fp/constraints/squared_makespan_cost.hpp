#ifndef FP_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP
#define FP_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Soft, weight 1: squaredMakespanCost — the SQUARE of how long each line
// stays busy, which is what balances work across lines instead of merely
// shortening the total. Raw cost per line = (end of its last job - line
// start)^2 in minutes^2; an empty line (and the unassigned pool) costs 0.
//
// The squaring lives in cost() rather than in `exponent` on purpose: the
// exponent shapes the raw cost of a unit before the weight, and here the
// square is the constraint's own definition, so a caller can still shape it
// further.
struct SquaredMakespanCost final : detail::PerLineConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SquaredMakespanCost(double weight = kDefaultWeight)
        : detail::PerLineConstraint("squaredMakespanCost", weight, kDefaultLevel)
    {
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

#endif  // FP_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP
