#ifndef PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_HPP
#define PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Hard (default, weight 1): renewableResourceCapacity — for every (renewable
// resource, day), how far the total requirement of the allocations busy that
// day exceeds its capacity. A capacity that refreshes daily, so this one is
// entirely about WHEN jobs run: it reads the whole date chain.
struct RenewableResourceCapacity final : ScheduleAggregateConstraint
{
    explicit RenewableResourceCapacity(double weight = 1.0)
        : ScheduleAggregateConstraint("renewableResourceCapacity",
                                      weight,
                                      /*hard=*/true)
    {
    }
    double evaluate(const ProjectJobSchedule &schedule,
                    const std::vector<int> &startDate,
                    const std::vector<int> &endDate) const override
    {
        usage_.assign(schedule.resources.size(), {});
        for (int a = 0; a < schedule.allocationCount(); ++a)
        {
            const int value = schedule.allocations[a].modeDelay;
            if (value < 0 || endDate[a] <= startDate[a])
                continue;
            for (const ResourceRequirement &requirement :
                 schedule.executionModeOf(a, value).resourceRequirements)
            {
                if (!schedule.resources[requirement.resource].renewable)
                    continue;
                std::vector<int> &perDay = usage_[requirement.resource];
                if (static_cast<int>(perDay.size()) < endDate[a])
                    perDay.resize(endDate[a], 0);
                for (int day = startDate[a]; day < endDate[a]; ++day)
                    perDay[day] += requirement.requirement;
            }
        }
        std::int64_t excess = 0;
        for (std::size_t r = 0; r < schedule.resources.size(); ++r)
        {
            const Resource &resource = schedule.resources[r];
            if (!resource.renewable)
                continue;
            for (int used : usage_[r])
            {
                if (used > resource.capacity)
                    excess += used - resource.capacity;
            }
        }
        return static_cast<double>(excess);
    }

private:
    mutable std::vector<std::vector<int>> usage_;  // scratch, (resource, day)
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_HPP
