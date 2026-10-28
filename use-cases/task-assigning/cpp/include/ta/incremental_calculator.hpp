#ifndef TA_INCREMENTAL_CALCULATOR_HPP
#define TA_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ta/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace ta
{

// The task-assigning implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint decomposes per employee list: the chained start times,
// the missing-skill count, the priority-weighted end times and the
// employee's final end time are all functions of ONE list's ordered
// contents. The calculator therefore caches one aggregate per list
//   (contribMissing, contribWeightedEnd, endTime)
// plus the global assigned-task count. beforeListChanged retracts the
// list's cached aggregates while it still holds its OLD contents;
// afterListChanged rebuilds them with a single O(listSize) walk — a move
// touches at most two lists, so a move costs O(delta), not O(solution).
//
// Tasks in no list (unassigned) contribute only the soft[0] penalty, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in
// the tests and under EnvironmentMode::FULL_ASSERT).
//
// resetWorkingSolution also interns two lookup tables so the hot per-move
// walk does no string comparisons:
//   missingByEmployeeType_[e][t]  missing skills of type t for employee e
//   durationByEmployeeTask_[e][i] duration of task i when employee e does it
class TaskAssigningIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<TaskAssigningSolution>
{
public:
    void resetWorkingSolution(TaskAssigningSolution &workingSolution) override;
    void beforeListChanged(int listIndex) override;
    void afterListChanged(int listIndex) override;
    optimaru::HardSoftScore calculateScore() override;

private:
    TaskAssigningSolution *solution_ = nullptr;

    // Interned lookups (built once per resetWorkingSolution).
    std::vector<std::vector<int>> missingByEmployeeType_;
    std::vector<std::vector<Minutes>> durationByEmployeeTask_;

    // Cached per-list aggregates (positive penalty units).
    std::vector<std::int64_t> contribMissing_;
    std::vector<std::int64_t> contribWeightedEnd_;
    std::vector<Minutes> endTime_;

    // Running totals (positive penalty units; signs applied in
    // calculateScore).
    std::int64_t missingTotal_ = 0;
    std::int64_t weightedEndTotal_ = 0;
    std::int64_t makespanSquaredTotal_ = 0;
    std::int64_t assignedCount_ = 0;

    // Rebuilds list l's aggregates from its current contents and adds them
    // to the totals.
    void insertList(int listIndex);
};

}  // namespace ta

#endif  // TA_INCREMENTAL_CALCULATOR_HPP
