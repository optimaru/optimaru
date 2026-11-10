#ifndef ALB_CONSTRAINTS_TASK_STATION_BOUNDS_COST_HPP
#define ALB_CONSTRAINTS_TASK_STATION_BOUNDS_COST_HPP

#include <vector>

#include "alb/constraints/common.hpp"

namespace alb {

// Hard, weight 1: taskStationBoundsCost — tasks outside their own station
// domain [min_station, max_station]. A task on station s costs
// max(0, min_station - s) + max(0, s - max_station) (no upper term without a
// max_station): the number of stations it sits outside its domain. Raw cost
// per station = the sum over its tasks; 0 = every task inside its domain.
struct TaskStationBoundsCost final : PerTaskConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TaskStationBoundsCost(double weight = kDefaultWeight)
        : PerTaskConstraint("taskStationBoundsCost", weight, kDefaultLevel) {}
    double taskCost(const AssemblyLineBalancingProblem& problem, int station,
                    int task) const override {
        const Task& t = problem.tasks[task];
        int outside = t.minStation > station ? t.minStation - station : 0;
        if (t.maxStation && station > *t.maxStation) outside += station - *t.maxStation;
        return static_cast<double>(outside);
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_TASK_STATION_BOUNDS_COST_HPP
