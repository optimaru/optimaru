#ifndef ST_OBJECTIVE_WEIGHTS_HPP
#define ST_OBJECTIVE_WEIGHTS_HPP

#include "st/constraints/class_gap_cost.hpp"
#include "st/constraints/slot/avoided_period_cost.hpp"
#include "st/constraints/slot/non_preferred_period_cost.hpp"
#include "st/constraints/teacher/teacher_free_day_cost.hpp"
#include "st/constraints/teacher/teacher_gap_cost.hpp"
#include "st/constraints/teacher/teacher_workday_cost.hpp"

namespace st::canon {

// The soft weights of the MONOLITHIC fast path, one per soft constraint and
// named like it (canon.ObjectiveWeights(teacher_gap_cost=40) in Python).
// Solver-config-level, never part of the model: the canonical Timetable
// carries no weights. The defaults are the soft components' kDefaultWeight
// (20/3/30/10/5/1), so a default-constructed ObjectiveWeights scores exactly
// like the default plugin assembly. A weight of 0 removes the term; soft
// score = -(weighted penalty sum).
//
// The plugin path (Solver::solve(table, calculator)) never reads these: the
// assembly's components carry their own weights.
struct ObjectiveWeights {
    double teacherGapCost = TeacherGapCost::kDefaultWeight;
    double teacherWorkdayCost = TeacherWorkdayCost::kDefaultWeight;
    double teacherFreeDayCost = TeacherFreeDayCost::kDefaultWeight;
    double classGapCost = ClassGapCost::kDefaultWeight;
    double avoidedPeriodCost = AvoidedPeriodCost::kDefaultWeight;
    double nonPreferredPeriodCost = NonPreferredPeriodCost::kDefaultWeight;
};

}  // namespace st::canon

#endif  // ST_OBJECTIVE_WEIGHTS_HPP
