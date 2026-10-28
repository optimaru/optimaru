#ifndef FP_CONSTRAINTS_MAX_END_DATE_TIME_HPP
#define FP_CONSTRAINTS_MAX_END_DATE_TIME_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Hard (default, weight 1): maxEndDateTime — the minutes a job runs past the
// deadline it must not miss. Unassigned jobs are silent here;
// maximizeJobsAssigned is the term that prices them.
struct MaxEndDateTime final : FpUnitConstraint
{
    explicit MaxEndDateTime(double weight = 1.0)
        : FpUnitConstraint("maxEndDateTime", weight, /*hard=*/true)
    {
    }
    void onReset(const PackagingSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const PackagingSchedule &schedule,
                int unit,
                const std::vector<int> & /*members*/) const override
    {
        if (detail::isPool(schedule, unit))
            return 0.0;
        std::int64_t late = 0;
        detail::walkChain(schedule,
                          unit,
                          detail::jobsOfUnit(schedule, unit),
                          [&](int jobIndex,
                              Minutes /*startCleaning*/,
                              Minutes /*startProduction*/,
                              Minutes end)
                          {
                              const Job &job = schedule.jobs[jobIndex];
                              if (job.maxEndTime < end)
                                  late += end - job.maxEndTime;
                          });
        return static_cast<double>(late);
    }
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_MAX_END_DATE_TIME_HPP
