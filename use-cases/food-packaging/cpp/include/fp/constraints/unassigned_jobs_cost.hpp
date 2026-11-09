#ifndef FP_CONSTRAINTS_UNASSIGNED_JOBS_COST_HPP
#define FP_CONSTRAINTS_UNASSIGNED_JOBS_COST_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Medium, weight 1: unassignedJobsCost — the production minutes of every job
// that is on no packaging LINE, so dropping a long job hurts more than
// dropping a short one. Raw cost (one global total) = the summed
// durationMinutes of the jobs in the unassigned pool or in no list at all,
// so 0 = every job is on a line. The exponent shapes that total. 0.2.0
// migration note: was maximizeJobsAssigned, soft at weight 10^10.
//
// The weight is in MEDIUM units (kMediumFold in domain.hpp folds medium into
// the engine's soft level as `medium * 10^10`); hard() makes it 1 hard per
// minute, soft() 1 soft per minute (outside the fold), and an explicit weight
// is in units of the level in effect (medium by default).
//
// Global rather than a unit constraint on the pool list, even though this port
// models the unassigned pool as one extra LIST: a job can also be in NO list
// at all (that is the state a solution starts in, before the construction
// heuristic runs), and the monolithic calculator counts it exactly the same
// way. "Not on a line" is therefore the condition, and it is not a property of
// any single unit.
//
// It keeps the assigned minutes per line unit, so a touched list costs
// O(its jobs) and the total is one subtraction.
struct UnassignedJobsCost final : FpGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit UnassignedJobsCost(double weight = kDefaultWeight)
        : FpGlobalConstraint("unassignedJobsCost", weight, kDefaultLevel)
    {
    }

    void
    reset(const PackagingSchedule &schedule,
          const std::vector<std::vector<int>> & /*members_per_unit*/) override
    {
        detail::requirePrepared(schedule);
        totalMinutes_ = 0;
        for (const Job &job : schedule.jobs)
            totalMinutes_ += job.durationMinutes;
        assignedPerLine_.assign(schedule.lineCount(), 0);
        assignedMinutes_ = 0;
        for (int line = 0; line < schedule.lineCount(); ++line)
        {
            assignedPerLine_[line] = minutesOn(schedule, line);
            assignedMinutes_ += assignedPerLine_[line];
        }
    }

    void on_unit_changed(const PackagingSchedule &schedule,
                       int unit,
                       const std::vector<int> & /*members*/) override
    {
        if (detail::isPool(schedule, unit))
            return;  // the pool is "not on a line" either way
        const std::int64_t minutes = minutesOn(schedule, unit);
        assignedMinutes_ += minutes - assignedPerLine_[unit];
        assignedPerLine_[unit] = minutes;
    }

    double cost() const override
    {
        return static_cast<double>(totalMinutes_ - assignedMinutes_);
    }

private:
    static std::int64_t minutesOn(const PackagingSchedule &schedule, int line)
    {
        std::int64_t minutes = 0;
        for (int jobIndex : detail::jobsOfUnit(schedule, line))
            minutes += schedule.jobs[jobIndex].durationMinutes;
        return minutes;
    }

    std::int64_t totalMinutes_ = 0;
    std::int64_t assignedMinutes_ = 0;
    std::vector<std::int64_t> assignedPerLine_;
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_UNASSIGNED_JOBS_COST_HPP
