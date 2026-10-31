#ifndef PJS_CONSTRAINTS_PROJECT_DELAY_COST_HPP
#define PJS_CONSTRAINTS_PROJECT_DELAY_COST_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Medium, weight 1: projectDelayCost — the days each project's SINK
// allocation ends past that project's critical-path end date
// (releaseDate + criticalPathDuration). Raw cost (one global total) = sum
// over projects of max(0, SINK endDate - criticalPathEndDate), in days, so
// 0 = every project ends on or before its critical-path end date. Projects
// finishing early cost nothing; only lateness counts. The exponent shapes
// the total. 0.2.0 migration note: was totalProjectDelay, soft at weight
// 10^9.
//
// The weight is in MEDIUM units (kMediumFold in domain.hpp folds medium into
// the engine's soft level as `medium * 10^9`); hard() makes it 1 hard per
// day, soft() 1 soft per day (outside the fold), and an explicit weight
// stays in medium units.
struct ProjectDelayCost final : ScheduleAggregateConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit ProjectDelayCost(double weight = kDefaultWeight)
        : ScheduleAggregateConstraint("projectDelayCost",
                                      weight,
                                      kDefaultLevel)
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

#endif  // PJS_CONSTRAINTS_PROJECT_DELAY_COST_HPP
