#ifndef ALB_CONSTRAINTS_FIXED_TASK_STATIONS_COST_HPP
#define ALB_CONSTRAINTS_FIXED_TASK_STATIONS_COST_HPP

#include <vector>

#include "alb/constraints/common.hpp"

namespace alb {

// Hard, weight 1: fixedTaskStationsCost — tasks with a fixed_station placed
// anywhere else. A task costs 1 while it is assigned to a station other than
// its fixed_station (a task without one never costs). Raw cost per station =
// the number of such tasks on it; 0 = every fixed task on its station.
//
// A penalty, not a pin: a seed may break it and its level can be flipped.
struct FixedTaskStationsCost final : PerTaskConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FixedTaskStationsCost(double weight = kDefaultWeight)
        : PerTaskConstraint("fixedTaskStationsCost", weight, kDefaultLevel) {}
    double taskCost(const AssemblyLineBalancingProblem& problem, int station,
                    int task) const override {
        const Task& t = problem.tasks[task];
        return t.fixedStation && *t.fixedStation != station ? 1.0 : 0.0;
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_FIXED_TASK_STATIONS_COST_HPP
