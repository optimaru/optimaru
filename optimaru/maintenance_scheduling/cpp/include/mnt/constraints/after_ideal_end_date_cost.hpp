#ifndef MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_COST_HPP
#define MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt {

// Soft, weight 1'000'000: afterIdealEndDateCost — the calendar days a job
// finishes LATE. Raw cost per (start date, crew) cell = the summed days by
// which its jobs' (exclusive) end date passes their idealEndDate; 0 = every
// job there ends on or before its ideal date (or has none). A thousand times
// more expensive than a tag conflict and a million times more than finishing
// early, which is what makes overdue maintenance the term the search clears
// first among the softs.
struct AfterIdealEndDateCost final : PerJobConstraint {
    static constexpr double kDefaultWeight = 1000000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AfterIdealEndDateCost(double weight = kDefaultWeight)
        : PerJobConstraint("afterIdealEndDateCost", weight, kDefaultLevel) {}
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        const Job& job = schedule.jobs[jobIndex];
        if (job.idealEndDate == kNoDate) return 0.0;
        const std::int64_t endDate =
                schedule.endDateFor(job.durationInDays, schedule.dateIndexOf(unit));
        return endDate > job.idealEndDate ? static_cast<double>(endDate - job.idealEndDate) : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_COST_HPP
