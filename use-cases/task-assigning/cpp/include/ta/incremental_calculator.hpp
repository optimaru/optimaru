#ifndef TA_INCREMENTAL_CALCULATOR_HPP
#define TA_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ta/domain.hpp"
#include "optimaru/objective.hpp"

namespace ta
{

// The task-assigning implementation of the engine's
// optimaru::Objective — the C++ analog of implementing
// Java's Objective instead of a ConstraintProvider.
//
// Every constraint decomposes per employee list: the chained start times,
// the missing-skill count, the priority-weighted end times and the
// employee's final end time are all functions of ONE list's ordered
// contents. The calculator therefore caches one aggregate per list
//   (contribMissing, contribWeightedEnd, endTime)
// plus the global assigned-task count. before_list_edit retracts the
// list's cached aggregates while it still holds its OLD contents;
// after_list_edit rebuilds them with a single O(list_size) walk — a move
// touches at most two lists, so a move costs O(delta), not O(solution).
//
// Tasks in no list (unassigned) contribute only the soft[0] penalty, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::score (checked in
// the tests and under ScoreChecks::ScoreChecks::Full).
//
// rebuild also interns two lookup tables so the hot per-move
// walk does no string comparisons:
//   missingByEmployeeType_[e][t]  missing skills of type t for employee e
//   durationByEmployeeTask_[e][i] duration of task i when employee e does it
class TaskAssigningIncrementalScoreCalculator final
    : public optimaru::Objective<TaskAssigningSolution>
{
public:
    void rebuild(TaskAssigningSolution &solution) override;
    void before_list_edit(int listIndex) override;
    void after_list_edit(int listIndex) override;
    optimaru::Score score() override;

private:
    TaskAssigningSolution *solution_ = nullptr;

    // Interned lookups (built once per rebuild).
    std::vector<std::vector<int>> missingByEmployeeType_;
    std::vector<std::vector<Minutes>> durationByEmployeeTask_;

    // Cached per-list aggregates (positive penalty units).
    std::vector<std::int64_t> contribMissing_;
    std::vector<std::int64_t> contribWeightedEnd_;
    std::vector<Minutes> endTime_;

    // Running totals (positive penalty units; signs applied in
    // score).
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
