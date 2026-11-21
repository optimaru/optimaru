#ifndef MNT_CONSTRAINTS_MIN_START_DATE_COST_HPP
#define MNT_CONSTRAINTS_MIN_START_DATE_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt {

// Hard, weight 1: minStartDateCost — the calendar days a job starts before
// the earliest date it may start on. Raw cost per (start date, crew) cell =
// the summed days by which its jobs' start date falls before their
// minStartDate; 0 = every job there starts in time (or has no minStartDate).
struct MinStartDateCost final : PerJobConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MinStartDateCost(double weight = kDefaultWeight)
        : PerJobConstraint("minStartDateCost", weight, kDefaultLevel) {}
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        const Job& job = schedule.jobs[jobIndex];
        if (job.minStartDate == kNoDate) return 0.0;
        const std::int64_t startDate = schedule.startDates[schedule.dateIndexOf(unit)];
        return startDate < job.minStartDate ? static_cast<double>(job.minStartDate - startDate)
                                            : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_MIN_START_DATE_COST_HPP
