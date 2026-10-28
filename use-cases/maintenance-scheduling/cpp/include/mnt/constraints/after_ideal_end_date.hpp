#ifndef MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_HPP
#define MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Soft (default, weight 1'000'000): afterIdealEndDate — one point per
// calendar day a job finishes LATE. A thousand times more expensive than a
// tag conflict and a million times more than finishing early, which is what
// makes overdue maintenance the term the search clears first among the softs.
struct AfterIdealEndDate final : PerJobConstraint
{
    explicit AfterIdealEndDate(double weight = 1000000.0)
        : PerJobConstraint("afterIdealEndDate", weight, /*hard=*/false)
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
        return endDate > job.idealEndDate
                   ? static_cast<double>(endDate - job.idealEndDate)
                   : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_AFTER_IDEAL_END_DATE_HPP
