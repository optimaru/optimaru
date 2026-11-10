#ifndef FP_CONSTRAINTS_MAX_END_TIME_COST_HPP
#define FP_CONSTRAINTS_MAX_END_TIME_COST_HPP

#include "fp/constraints/common.hpp"

namespace fp {

// Hard, weight 1: maxEndTimeCost — the minutes a job runs past the deadline
// it must not miss (Job::maxEndTime). Raw cost per line = the sum over its
// jobs of max(0, end - maxEndTime), so 0 = every job on the line ends by its
// deadline. Unassigned jobs are silent here; unassignedJobsCost is the term
// that prices them.
struct MaxEndTimeCost final : detail::PerLineConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxEndTimeCost(double weight = kDefaultWeight)
        : detail::PerLineConstraint("maxEndTimeCost", weight, kDefaultLevel) {}
    double cost(const PackagingSchedule& schedule, int unit,
                const std::vector<int>& /*members*/) const override {
        if (detail::isPool(schedule, unit)) return 0.0;
        std::int64_t late = 0;
        detail::walkChain(schedule, unit, detail::jobsOfUnit(schedule, unit),
                          [&](int jobIndex, Minutes /*startCleaning*/, Minutes /*startProduction*/,
                              Minutes end) {
                              const Job& job = schedule.jobs[jobIndex];
                              if (job.maxEndTime < end) late += end - job.maxEndTime;
                          });
        return static_cast<double>(late);
    }
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_MAX_END_TIME_COST_HPP
