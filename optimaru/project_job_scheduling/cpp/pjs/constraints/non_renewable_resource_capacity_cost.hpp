#ifndef PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_CAPACITY_COST_HPP
#define PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_CAPACITY_COST_HPP

#include "pjs/constraints/common.hpp"

namespace pjs {

// Hard, weight 1: nonRenewableResourceCapacityCost — for every
// non-renewable resource, how far the total requirement of all assigned
// execution modes exceeds its capacity. Raw cost (one global total) =
// sum over non-renewable resources of max(0, total requirement - capacity),
// in resource units, so 0 = every stock is within its capacity. A stock that
// is consumed once, so WHEN a job runs is irrelevant: this is the one
// component that skips the date chain. The exponent shapes the total.
struct NonRenewableResourceCapacityCost final : ScheduleAggregateConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit NonRenewableResourceCapacityCost(double weight = kDefaultWeight)
        : ScheduleAggregateConstraint("nonRenewableResourceCapacityCost", weight, kDefaultLevel) {}
    bool needsDates() const override { return false; }
    double evaluate(const ProjectJobSchedule& schedule, const std::vector<int>& /*startDate*/,
                    const std::vector<int>& /*endDate*/) const override {
        usage_.assign(schedule.resources.size(), 0);
        for (int a = 0; a < schedule.allocationCount(); ++a) {
            const int value = schedule.allocations[a].modeDelay;
            if (value < 0) continue;  // unassigned: no resource usage
            for (const ResourceRequirement& requirement :
                 schedule.executionModeOf(a, value).resourceRequirements) {
                if (!schedule.resources[requirement.resource].renewable)
                    usage_[requirement.resource] += requirement.requirement;
            }
        }
        std::int64_t excess = 0;
        for (std::size_t r = 0; r < schedule.resources.size(); ++r) {
            const Resource& resource = schedule.resources[r];
            if (!resource.renewable && usage_[r] > resource.capacity)
                excess += usage_[r] - resource.capacity;
        }
        return static_cast<double>(excess);
    }

private:
    mutable std::vector<std::int64_t> usage_;  // scratch, per resource
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_CAPACITY_COST_HPP
