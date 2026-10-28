#ifndef MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_HPP
#define MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Soft (default, weight 1): beforeIdealEndDate — one point per calendar day a
// job finishes EARLY. The cheapest of the three soft terms on purpose: doing
// maintenance ahead of the ideal date wastes remaining life, but only mildly.
struct BeforeIdealEndDate final : PerJobConstraint
{
    explicit BeforeIdealEndDate(double weight = 1.0)
        : PerJobConstraint("beforeIdealEndDate", weight, /*hard=*/false)
    {
    }
    double jobCost(const MaintenanceSchedule &schedule,
                   int unit,
                   int jobIndex) const override
    {
        const Job &job = schedule.jobs[jobIndex];
        if (job.idealEndDate == kNoDate)
            return 0.0;
        const std::int64_t endDate = schedule.endDateFor(
            job.durationInDays, schedule.dateIndexOf(unit));
        return endDate < job.idealEndDate
                   ? static_cast<double>(job.idealEndDate - endDate)
                   : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_BEFORE_IDEAL_END_DATE_HPP
