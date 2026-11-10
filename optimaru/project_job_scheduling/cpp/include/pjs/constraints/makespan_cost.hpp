#ifndef PJS_CONSTRAINTS_MAKESPAN_COST_HPP
#define PJS_CONSTRAINTS_MAKESPAN_COST_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Soft, weight 1: makespanCost — the latest SINK end date, i.e. when the
// last project finishes. Raw cost (one global value, a max rather than a
// total) = max over SINK allocations of endDate, a day index counted from
// day 0 (release dates do not move it), so 0 = every project ends on day 0
// (or there is no SINK at all). One penalty for the whole schedule, not one
// per project, so shortening any project other than the last one is free on
// this term. The exponent shapes that value.
struct MakespanCost final : ScheduleAggregateConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MakespanCost(double weight = kDefaultWeight)
        : ScheduleAggregateConstraint("makespanCost", weight, kDefaultLevel)
    {
    }
    double evaluate(const ProjectJobSchedule &schedule,
                    const std::vector<int> & /*startDate*/,
                    const std::vector<int> &endDate) const override
    {
        std::int64_t makespan = 0;
        bool seenSink = false;
        for (int a = 0; a < schedule.allocationCount(); ++a)
        {
            if (schedule.jobTypeOf(a) != JobType::SINK)
                continue;
            makespan = seenSink ? std::max<std::int64_t>(makespan, endDate[a])
                                : endDate[a];
            seenSink = true;
        }
        return static_cast<double>(makespan);
    }
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_MAKESPAN_COST_HPP
