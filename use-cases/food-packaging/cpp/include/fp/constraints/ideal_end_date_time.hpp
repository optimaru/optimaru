#ifndef FP_CONSTRAINTS_IDEAL_END_DATE_TIME_HPP
#define FP_CONSTRAINTS_IDEAL_END_DATE_TIME_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Medium (default, weight 1 medium level): idealEndDateTime — the minutes a
// job runs past the date it should ideally be done by. Missing the ideal date
// is a cost, missing maxEndTime is a violation, which is why the two sit on
// different levels with otherwise identical shapes.
//
// The medium level is folded into the engine's soft level as
// `medium * kMediumUnit` (see domain.hpp), which is what the default weight
// carries; the component's own cost stays plain minutes.
struct IdealEndDateTime final : FpUnitConstraint
{
    explicit IdealEndDateTime(double weight = static_cast<double>(kMediumUnit))
        : FpUnitConstraint("idealEndDateTime", weight, /*hard=*/false)
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
                              if (job.idealEndTime < end)
                                  late += end - job.idealEndTime;
                          });
        return static_cast<double>(late);
    }
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_IDEAL_END_DATE_TIME_HPP
