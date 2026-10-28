#ifndef TA_SCORE_CALCULATOR_HPP
#define TA_SCORE_CALCULATOR_HPP

#include "ta/domain.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace ta
{

// Port of org.acme.taskassigning.solver.TaskAssigningConstraintProvider as a
// full-recompute reference: walks every employee's task list, re-derives the
// shadow variables and sums the four Bendable levels. It is the correctness
// oracle for the incremental calculator (checked in the tests and under
// EnvironmentMode::FULL_ASSERT) and the authoritative final scoring.
class ScoreCalculator
{
public:
    // Recomputes every task's shadow variables (employee, startTime) from
    // the current employee task lists, mirroring the Java shadow variables
    // (@InverseRelationShadowVariable + startTimeSupplier). Must be called
    // before reading Task::employee / Task::startTime whenever lists change.
    static void updateShadowVariables(TaskAssigningSolution &solution);

    // Recomputes shadow variables, then the Bendable score, storing it in
    // solution.score and returning it. Constraints (all penalties):
    //   hard[0]  noMissingSkills          missing skill count per task
    //   soft[0]  minimizeUnassignedTasks  1 per unassigned task
    //   soft[1]  minimizeMakespan         endTime^2 per employee
    //   soft[2]  criticalPriorityTaskEndTime  endTime x 4
    //            majorPriorityTaskEndTime     endTime x 2
    //            minorPriorityTaskEndTime     endTime x 1
    static Score calculateScore(TaskAssigningSolution &solution);

    // calculateScore mapped into the engine's HardSoftScore (see the mapping
    // rule in ta/domain.hpp).
    static optimaru::HardSoftScore
    calculateEngineScore(TaskAssigningSolution &solution);
};

}  // namespace ta

#endif  // TA_SCORE_CALCULATOR_HPP
