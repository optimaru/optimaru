#ifndef MNT_CONSTRAINTS_MAX_END_DATE_HPP
#define MNT_CONSTRAINTS_MAX_END_DATE_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Hard (default, weight 1): maxEndDate — one point per calendar day a job
// ends after the latest date it may end on.
struct MaxEndDate final : PerJobConstraint
{
    explicit MaxEndDate(double weight = 1.0)
        : PerJobConstraint("maxEndDate", weight, /*hard=*/true)
    {
    }
    double jobCost(const MaintenanceSchedule &schedule,
                   int unit,
                   int jobIndex) const override
    {
        const Job &job = schedule.jobs[jobIndex];
        if (job.maxEndDate == kNoDate)
            return 0.0;
        const std::int64_t endDate = schedule.endDateFor(
            job.durationInDays, schedule.dateIndexOf(unit));
        return endDate > job.maxEndDate
                   ? static_cast<double>(endDate - job.maxEndDate)
                   : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_MAX_END_DATE_HPP
