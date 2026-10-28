#ifndef MNT_CONSTRAINTS_MIN_START_DATE_HPP
#define MNT_CONSTRAINTS_MIN_START_DATE_HPP

#include "mnt/constraints/common.hpp"

namespace mnt
{

// Hard (default, weight 1): minStartDate — one point per calendar day a job
// starts before the earliest date it may start on.
struct MinStartDate final : PerJobConstraint
{
    explicit MinStartDate(double weight = 1.0)
        : PerJobConstraint("minStartDate", weight, /*hard=*/true)
    {
    }
    double jobCost(const MaintenanceSchedule &schedule,
                   int unit,
                   int jobIndex) const override
    {
        const Job &job = schedule.jobs[jobIndex];
        if (job.minStartDate == kNoDate)
            return 0.0;
        const std::int64_t startDate
            = schedule.startDates[schedule.dateIndexOf(unit)];
        return startDate < job.minStartDate
                   ? static_cast<double>(job.minStartDate - startDate)
                   : 0.0;
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_MIN_START_DATE_HPP
