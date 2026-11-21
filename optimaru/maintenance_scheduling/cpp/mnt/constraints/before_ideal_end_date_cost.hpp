#ifndef MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_COST_HPP
#define MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt {

// Soft, weight 1: beforeIdealEndDateCost — the calendar days a job finishes
// EARLY. Raw cost per (start date, crew) cell = the summed days by which its
// jobs' (exclusive) end date falls before their idealEndDate; 0 = every job
// there ends on or after its ideal date (or has none). The cheapest of the
// three soft terms on purpose: doing maintenance ahead of the ideal date
// wastes remaining life, but only mildly.
struct BeforeIdealEndDateCost final : PerJobConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit BeforeIdealEndDateCost(double weight = kDefaultWeight)
        : PerJobConstraint("beforeIdealEndDateCost", weight, kDefaultLevel) {}
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        const Job& job = schedule.jobs[jobIndex];
        if (job.idealEndDate == kNoDate) return 0.0;
        const std::int64_t endDate =
                schedule.endDateFor(job.durationInDays, schedule.dateIndexOf(unit));
        return endDate < job.idealEndDate ? static_cast<double>(job.idealEndDate - endDate) : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_COST_HPP
