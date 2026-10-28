#ifndef FP_CONSTRAINTS_OPERATOR_CLEANING_CONFLICT_HPP
#define FP_CONSTRAINTS_OPERATOR_CLEANING_CONFLICT_HPP

#include "fp/constraints/common.hpp"

namespace fp
{

// Hard (default, weight 1): operatorCleaningConflict — the minutes two
// cleaning windows overlap on lines that share one operator. A person cannot
// clean two machines at once.
//
// Global because the pairs span units: the conflicting jobs sit on DIFFERENT
// lines (two jobs on the same line share its operator too, but their windows
// are strictly sequential, so they can never overlap).
//
// The cleaning windows depend on the whole chain of each line, so this
// component recomputes them lazily once per score rather than maintaining a
// delta: any notification marks it dirty, and cost() rebuilds the windows and
// the pair sum. cost() runs once per score, so a move costs O(jobs) plus the
// provider's own O(jobs^2) pair scan — the same shape as the full recompute,
// which is the trade this assembly path is for.
struct OperatorCleaningConflict final : FpGlobalConstraint
{
    explicit OperatorCleaningConflict(double weight = 1.0)
        : FpGlobalConstraint("operatorCleaningConflict", weight, /*hard=*/true)
    {
    }

    void
    reset(const PackagingSchedule &schedule,
          const std::vector<std::vector<int>> & /*membersPerUnit*/) override
    {
        detail::requirePrepared(schedule);
        schedule_ = &schedule;
        dirty_ = true;
    }
    void onUnitChanged(const PackagingSchedule &schedule,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        schedule_ = &schedule;
        dirty_ = true;
    }
    double cost() const override
    {
        if (!dirty_)
            return value_;
        const PackagingSchedule &schedule = *schedule_;
        const int jobCount = static_cast<int>(schedule.jobs.size());
        startCleaning_.assign(jobCount, kNoTime);
        startProduction_.assign(jobCount, kNoTime);
        lineOf_.assign(jobCount, -1);
        for (int line = 0; line < schedule.lineCount(); ++line)
        {
            detail::walkChain(schedule,
                              line,
                              detail::jobsOfUnit(schedule, line),
                              [&](int jobIndex,
                                  Minutes cleaning,
                                  Minutes production,
                                  Minutes /*end*/)
                              {
                                  startCleaning_[jobIndex] = cleaning;
                                  startProduction_[jobIndex] = production;
                                  lineOf_[jobIndex] = line;
                              });
        }
        // forEachUniquePair, verbatim: every unordered pair of assigned jobs
        // whose lines share an operator (the unassigned operator groups with
        // itself too, exactly like the Java Joiners.equal).
        std::int64_t overlap = 0;
        for (int a = 0; a < jobCount; ++a)
        {
            if (lineOf_[a] < 0)
                continue;
            for (int b = a + 1; b < jobCount; ++b)
            {
                if (lineOf_[b] < 0)
                    continue;
                if (schedule.lines[lineOf_[a]].operatorIndex
                    != schedule.lines[lineOf_[b]].operatorIndex)
                    continue;
                const Minutes start
                    = std::max(startCleaning_[a], startCleaning_[b]);
                const Minutes end
                    = std::min(startProduction_[a], startProduction_[b]);
                if (start < end)
                    overlap += end - start;
            }
        }
        value_ = static_cast<double>(overlap);
        dirty_ = false;
        return value_;
    }

private:
    const PackagingSchedule *schedule_ = nullptr;
    mutable std::vector<Minutes> startCleaning_;
    mutable std::vector<Minutes> startProduction_;
    mutable std::vector<int> lineOf_;
    mutable double value_ = 0.0;
    mutable bool dirty_ = true;
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_OPERATOR_CLEANING_CONFLICT_HPP
