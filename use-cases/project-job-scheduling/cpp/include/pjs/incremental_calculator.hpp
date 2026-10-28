#ifndef PJS_INCREMENTAL_CALCULATOR_HPP
#define PJS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include "pjs/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace pjs
{

// The project-job-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// The hard part of this domain is the Java shadow-variable chain
// (predecessorsDoneDate -> startDate -> endDate): changing one allocation's
// (executionMode, delay) can shift the dates of its whole successor
// subgraph, and with them the renewable resource profile, the project delays
// and the makespan. The calculator therefore keeps
//   - the recorded value / decoded mode / start / end per allocation,
//   - the non-renewable usage per resource (date-independent) and the
//     renewable usage per (resource, day), each with a running
//     over-capacity total,
//   - the per-sink project-delay total (medium) and a multiset of sink end
//     dates (soft = max).
// All work happens in afterVariableChanged: it diffs against the recorded
// state, so it is correct under every hook sequence the engine produces
// (change, swap, ruin's unassign to -1, snapshot restores, rollbacks via
// inverse writes). Date changes are re-propagated over ONLY the affected
// successor subgraph, in topological order via a min-heap on the topological
// position, and propagation stops at nodes whose end date is unchanged — a
// move costs O(delta), not O(solution).
//
// Unassigned allocations (modeDelay == -1) pass dates through with delay 0 /
// duration 0 and contribute no resource usage (the Java suppliers'
// null-handling), so the construction heuristic scores partially
// initialized solutions. The full-recompute reference is
// ScoreCalculator::calculateScore (checked in the tests and under
// EnvironmentMode::FULL_ASSERT).
class ProjectJobSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<ProjectJobSchedule>
{
public:
    void resetWorkingSolution(ProjectJobSchedule &workingSolution) override;
    void beforeVariableChanged(int allocationIndex) override;
    void afterVariableChanged(int allocationIndex) override;
    optimaru::HardSoftScore calculateScore() override;

private:
    ProjectJobSchedule *schedule_ = nullptr;

    // Recorded state per allocation (as of the last processed hook).
    std::vector<int> assign_;   // planning value (-1 unassigned)
    std::vector<int> modeIdx_;  // decoded execution mode index (-1 unassigned)
    std::vector<int> start_;
    std::vector<int> end_;

    // Resource usage and running over-capacity totals (positive penalty
    // units; signs applied in calculateScore).
    std::vector<std::int64_t> nonRenewableUsage_;   // [resource]
    std::vector<std::vector<int>> renewableUsage_;  // [resource][day]
    std::int64_t hardNonRenewable_ = 0;
    std::int64_t hardRenewable_ = 0;

    // medium: sum over sinks of max(0, end - criticalPathEnd).
    std::int64_t mediumTotal_ = 0;
    // soft = max sink end date: recomputed in calculateScore by scanning the
    // (few, one per project) sink allocations against end_ — profiling
    // showed the previous std::multiset spent more time on tree-node
    // allocation churn per date change than the scan costs per score call.
    std::vector<int> sinkAllocations_;

    // Propagation scratch: topological position per allocation, a "queued"
    // flag, and the (position, allocation) min-heap + touched list reused
    // across propagations (member buffers keep the hot path allocation-free;
    // see afterVariableChanged).
    std::vector<int> topologicalPosition_;
    std::vector<char> queued_;
    std::vector<std::pair<int, int>> propagationHeap_;
    std::vector<int> touched_;

    // Adds (sign=+1) or retracts (sign=-1) the renewable usage of mode
    // `modeIndex` of `allocationIndex` over the days [startDate, endDate),
    // maintaining hardRenewable_. No-op for modeIndex < 0.
    void applyRenewable(int allocationIndex,
                        int modeIndex,
                        int startDate,
                        int endDate,
                        int sign);
    // Same for the non-renewable (date-independent) usage.
    void applyNonRenewable(int allocationIndex, int modeIndex, int sign);
    // Rewrites the recorded dates of an allocation, maintaining the sink
    // aggregates (mediumTotal_, sinkEndDates_).
    void setRecordedDates(int allocationIndex, int startDate, int endDate);
    // max(0, max over predecessors of recorded end) — the Java
    // predecessorsDoneDateSupplier against the recorded state.
    int predecessorsDoneDate(int allocationIndex) const;
};

}  // namespace pjs

#endif  // PJS_INCREMENTAL_CALCULATOR_HPP
