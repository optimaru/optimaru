#ifndef ALB_CONSTRAINTS_FORBIDDEN_TASK_STATIONS_COST_HPP
#define ALB_CONSTRAINTS_FORBIDDEN_TASK_STATIONS_COST_HPP

#include <algorithm>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb
{

// Hard, weight 1: forbiddenTaskStationsCost — tasks on one of their
// forbidden_stations. A task costs 1 while it sits on a station its list
// names (a station listed twice still costs 1). Raw cost per station = the
// number of such tasks on it; 0 = no task on a forbidden station.
//
// The lookup is a binary search in the sorted unique copy prepare() builds;
// the component is scoped to the stations some task forbids.
struct ForbiddenTaskStationsCost final : PerTaskConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ForbiddenTaskStationsCost(double weight = kDefaultWeight)
        : PerTaskConstraint("forbiddenTaskStationsCost", weight, kDefaultLevel)
    {
    }
    void onReset(const AssemblyLineBalancingProblem &problem) override
    {
        PerTaskConstraint::onReset(problem);
        scoped_.clear();
        for (const std::vector<int> &forbidden : problem.forbiddenSorted)
        {
            for (int station : forbidden)
            {
                if (station >= 0 && station < problem.stationCount)
                    scoped_.push_back(station);
            }
        }
        std::sort(scoped_.begin(), scoped_.end());
        scoped_.erase(std::unique(scoped_.begin(), scoped_.end()), scoped_.end());
    }
    double taskCost(const AssemblyLineBalancingProblem &problem,
                    int station,
                    int task) const override
    {
        const std::vector<int> &forbidden = problem.forbiddenSorted[task];
        return std::binary_search(forbidden.begin(), forbidden.end(), station) ? 1.0 : 0.0;
    }
    const std::vector<int> *scopedUnits() const override { return &scoped_; }

private:
    std::vector<int> scoped_;
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_FORBIDDEN_TASK_STATIONS_COST_HPP
