#ifndef FP_CONSTRAINTS_MAXIMIZE_JOBS_ASSIGNED_HPP
#define FP_CONSTRAINTS_MAXIMIZE_JOBS_ASSIGNED_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Medium (default, weight 1 medium level): maximizeJobsAssigned — the
// production minutes of every job that is on no packaging LINE, so dropping a
// long job hurts more than dropping a short one.
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
struct MaximizeJobsAssigned final : FpGlobalConstraint
{
    explicit MaximizeJobsAssigned(double weight
                                  = static_cast<double>(kMediumUnit))
        : FpGlobalConstraint("maximizeJobsAssigned", weight, /*hard=*/false)
    {
    }

    void
    reset(const PackagingSchedule &schedule,
          const std::vector<std::vector<int>> & /*membersPerUnit*/) override
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

    void onUnitChanged(const PackagingSchedule &schedule,
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

#endif  // FP_CONSTRAINTS_MAXIMIZE_JOBS_ASSIGNED_HPP
