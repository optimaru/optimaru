#ifndef PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_COST_HPP
#define PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_COST_HPP

#include "pjs/constraints/common.hpp"

namespace pjs
{

// Hard, weight 1: renewableResourceCapacityCost — for every (renewable
// resource, day), how far the total requirement of the allocations busy that
// day exceeds its capacity. Raw cost (one global total) = sum over (renewable
// resource, day) of max(0, requirement busy that day - capacity), in
// resource-unit-days, so 0 = no resource is over capacity on any day. A
// capacity that refreshes daily, so this one is entirely about WHEN jobs run:
// it reads the whole date chain. The exponent shapes the total.
// 0.2.0 migration note: was renewableResourceCapacity.
struct RenewableResourceCapacityCost final : ScheduleAggregateConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RenewableResourceCapacityCost(double weight = kDefaultWeight)
        : ScheduleAggregateConstraint("renewableResourceCapacityCost",
                                      weight,
                                      kDefaultLevel)
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

#endif  // PJS_CONSTRAINTS_RENEWABLE_RESOURCE_CAPACITY_COST_HPP
