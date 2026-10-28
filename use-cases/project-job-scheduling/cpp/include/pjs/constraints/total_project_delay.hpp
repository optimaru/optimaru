#ifndef PJS_CONSTRAINTS_TOTAL_PROJECT_DELAY_HPP
#define PJS_CONSTRAINTS_TOTAL_PROJECT_DELAY_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Medium (default, weight 1 medium level): totalProjectDelay — the days each
// project's SINK allocation ends past that project's critical-path end date
// (releaseDate + criticalPathDuration). Projects finishing early cost
// nothing; only lateness counts.
//
// The medium level is folded into the engine's soft level as
// `medium * kMediumUnit` (see domain.hpp), which is what the default weight
// carries; the component's own cost stays plain days.
struct TotalProjectDelay final : ScheduleAggregateConstraint
{
    explicit TotalProjectDelay(double weight = static_cast<double>(kMediumUnit))
        : ScheduleAggregateConstraint("totalProjectDelay",
                                      weight,
                                      /*hard=*/false)
    {
    }
    double evaluate(const ProjectJobSchedule &schedule,
                    const std::vector<int> & /*startDate*/,
                    const std::vector<int> &endDate) const override
    {
        std::int64_t delay = 0;
        for (int a = 0; a < schedule.allocationCount(); ++a)
        {
            if (schedule.jobTypeOf(a) != JobType::SINK)
                continue;
            const int late
                = endDate[a] - schedule.projectCriticalPathEndDateOf(a);
            if (late > 0)
                delay += late;
        }
        return static_cast<double>(delay);
    }
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_TOTAL_PROJECT_DELAY_HPP
