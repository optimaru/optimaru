#ifndef TA_SCORE_CALCULATOR_HPP
#define TA_SCORE_CALCULATOR_HPP

#include "ta/domain.hpp"
#include "optimaru/score/score.hpp"

namespace ta {

// Full-recompute reference for the six constraints: walks every employee's
// task list, recomputes the derived task fields and sums the four score
// levels. It is the correctness
// oracle for the incremental calculator (checked in the tests and under
// ScoreChecks::ScoreChecks::Full) and the authoritative final scoring.
class ScoreCalculator {
public:
    // Recomputes every task's derived fields (employee, startTime) from
    // the current employee task lists. Must be called
    // before reading Task::employee / Task::startTime whenever lists change.
    static void updateShadowVariables(TaskAssigningSolution& solution);

    // Recomputes the derived fields, then the four-level score, storing it in
    // solution.score and returning it. Constraints (all penalties, at the
    // components' default weights):
    //   hard[0]  requiredSkillCost        missing skill count per task
    //   soft[0]  unassignedTasksCost      1 per unassigned task (medium)
    //   soft[1]  squaredMakespanCost      endTime^2 per employee
    //   soft[2]  criticalPriorityTaskEndTimeCost  endTime x 4
    //            majorPriorityTaskEndTimeCost     endTime x 2
    //            minorPriorityTaskEndTimeCost     endTime x 1
    static Score score(TaskAssigningSolution& solution);

    // score mapped into the engine's Score through the
    // domain's medium fold (see the mapping rule in ta/domain.hpp).
    static optimaru::Score calculateEngineScore(TaskAssigningSolution& solution);
};

}  // namespace ta

#endif  // TA_SCORE_CALCULATOR_HPP
