#ifndef PJS_CONSTRAINTS_TOTAL_MAKESPAN_HPP
#define PJS_CONSTRAINTS_TOTAL_MAKESPAN_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Soft (default, weight 1): totalMakespan — the latest SINK end date, i.e.
// when the last project finishes. One penalty for the whole schedule (the
// Java groupBy(max)), not one per project, so shortening any project other
// than the last one is free on this term.
struct TotalMakespan final : ScheduleAggregateConstraint
{
    explicit TotalMakespan(double weight = 1.0)
        : ScheduleAggregateConstraint("totalMakespan", weight, /*hard=*/false)
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

#endif  // PJS_CONSTRAINTS_TOTAL_MAKESPAN_HPP
