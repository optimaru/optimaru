#ifndef FP_CONSTRAINTS_AFTER_IDEAL_END_TIME_COST_HPP
#define FP_CONSTRAINTS_AFTER_IDEAL_END_TIME_COST_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Medium, weight 1: afterIdealEndTimeCost — the minutes a job runs past the
// time it should ideally be done by (Job::idealEndTime). Raw cost per line =
// the sum over its jobs of max(0, end - idealEndTime), so 0 = every job on
// the line ends by its ideal time. Missing the ideal time is a cost, missing
// maxEndTime is a violation, which is why the two sit on different levels
// with otherwise identical shapes. 0.2.0 migration note: was
// idealEndDateTime, soft at weight 10^10.
//
// The weight is in MEDIUM units: the domain folds the medium level into the
// engine's soft level as `medium * kMediumUnit` (kMediumUnit = 10^10,
// kMediumFold in domain.hpp), and the decomposed calculator applies that
// fold, so the component carries no 10^10 factor. hard() makes it 1 hard per
// minute, soft() 1 soft per minute (outside the fold), and an explicit weight
// is in units of the level in effect (medium by default).
struct AfterIdealEndTimeCost final : detail::PerLineConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit AfterIdealEndTimeCost(double weight = kDefaultWeight)
        : detail::PerLineConstraint("afterIdealEndTimeCost", weight, kDefaultLevel)
    {
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

#endif  // FP_CONSTRAINTS_AFTER_IDEAL_END_TIME_COST_HPP
