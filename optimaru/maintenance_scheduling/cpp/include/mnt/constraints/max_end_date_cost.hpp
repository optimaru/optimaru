#ifndef MNT_CONSTRAINTS_MAX_END_DATE_COST_HPP
#define MNT_CONSTRAINTS_MAX_END_DATE_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt {

// Hard, weight 1: maxEndDateCost — the calendar days a job ends after the
// latest date it may end on. Raw cost per (start date, crew) cell = the
// summed days by which its jobs' (exclusive) end date passes their
// maxEndDate; 0 = every job there ends in time (or has no maxEndDate).
struct MaxEndDateCost final : PerJobConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxEndDateCost(double weight = kDefaultWeight)
        : PerJobConstraint("maxEndDateCost", weight, kDefaultLevel) {}
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        const Job& job = schedule.jobs[jobIndex];
        if (job.maxEndDate == kNoDate) return 0.0;
        const std::int64_t endDate =
                schedule.endDateFor(job.durationInDays, schedule.dateIndexOf(unit));
        return endDate > job.maxEndDate ? static_cast<double>(endDate - job.maxEndDate) : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_MAX_END_DATE_COST_HPP
