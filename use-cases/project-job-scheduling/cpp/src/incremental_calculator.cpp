#include "pjs/incremental_calculator.hpp"

#include <algorithm>
#include <functional>
#include <utility>

#include "pjs/score_calculator.hpp"

namespace pjs
{

namespace
{
inline std::int64_t overCapacity(std::int64_t used, int capacity)
{
    return used > capacity ? used - capacity : 0;
}
}  // namespace

void ProjectJobSchedulingIncrementalScoreCalculator::resetWorkingSolution(
    ProjectJobSchedule &workingSolution)
{
    schedule_ = &workingSolution;
    ScoreCalculator::prepare(workingSolution);
    // The full recompute is the semantic reference; reuse it to derive the
    // dates, then rebuild the internal aggregates from scratch.
    ScoreCalculator::updateDates(workingSolution);

    const int allocationCount = workingSolution.allocationCount();
    const int resourceCount
        = static_cast<int>(workingSolution.resources.size());

    assign_.assign(allocationCount, -1);
    modeIdx_.assign(allocationCount, -1);
    start_.assign(allocationCount, 0);
    end_.assign(allocationCount, 0);
    nonRenewableUsage_.assign(resourceCount, 0);
    renewableUsage_.assign(resourceCount, {});
    hardNonRenewable_ = 0;
    hardRenewable_ = 0;
    mediumTotal_ = 0;
    sinkAllocations_.clear();
    queued_.assign(allocationCount, 0);
    propagationHeap_.clear();
    propagationHeap_.reserve(allocationCount);
    touched_.clear();
    touched_.reserve(allocationCount);
    topologicalPosition_.assign(allocationCount, 0);
    for (int pos = 0;
         pos < static_cast<int>(workingSolution.topologicalOrder.size());
         ++pos)
    {
        topologicalPosition_[workingSolution.topologicalOrder[pos]] = pos;
    }

    for (int a = 0; a < allocationCount; ++a)
    {
        const Allocation &allocation = workingSolution.allocations[a];
        const int value = allocation.modeDelay;
        assign_[a] = value;
        modeIdx_[a] = value < 0
                          ? -1
                          : workingSolution.executionModeIndexOfValue(a, value);
        start_[a] = allocation.startDate;
        end_[a] = allocation.endDate;
        applyNonRenewable(a, modeIdx_[a], +1);
        applyRenewable(a, modeIdx_[a], start_[a], end_[a], +1);
        if (workingSolution.jobTypeOf(a) == JobType::SINK)
        {
            mediumTotal_ += std::max(
                0, end_[a] - workingSolution.projectCriticalPathEndDateOf(a));
            sinkAllocations_.push_back(a);
        }
    }
}

void ProjectJobSchedulingIncrementalScoreCalculator::beforeVariableChanged(
    int /*allocationIndex*/)
{
    // All bookkeeping diffs against the recorded state in
    // afterVariableChanged; nothing to retract here.
}

void ProjectJobSchedulingIncrementalScoreCalculator::applyRenewable(
    int allocationIndex, int modeIndex, int startDate, int endDate, int sign)
{
    if (modeIndex < 0 || endDate <= startDate)
        return;
    const Allocation &allocation = schedule_->allocations[allocationIndex];
    const ExecutionMode &mode
        = schedule_->jobs[allocation.job].executionModes[modeIndex];
    for (const ResourceRequirement &req : mode.resourceRequirements)
    {
        const Resource &resource = schedule_->resources[req.resource];
        if (!resource.renewable)
            continue;
        std::vector<int> &usage = renewableUsage_[req.resource];
        if (static_cast<int>(usage.size()) < endDate)
            usage.resize(endDate, 0);
        for (int d = startDate; d < endDate; ++d)
        {
            const int oldUsed = usage[d];
            const int newUsed = oldUsed + sign * req.requirement;
            hardRenewable_ += overCapacity(newUsed, resource.capacity)
                              - overCapacity(oldUsed, resource.capacity);
            usage[d] = newUsed;
        }
    }
}

void ProjectJobSchedulingIncrementalScoreCalculator::applyNonRenewable(
    int allocationIndex, int modeIndex, int sign)
{
    if (modeIndex < 0)
        return;
    const Allocation &allocation = schedule_->allocations[allocationIndex];
    const ExecutionMode &mode
        = schedule_->jobs[allocation.job].executionModes[modeIndex];
    for (const ResourceRequirement &req : mode.resourceRequirements)
    {
        const Resource &resource = schedule_->resources[req.resource];
        if (resource.renewable)
            continue;
        const std::int64_t oldUsed = nonRenewableUsage_[req.resource];
        const std::int64_t newUsed = oldUsed + sign * req.requirement;
        hardNonRenewable_ += overCapacity(newUsed, resource.capacity)
                             - overCapacity(oldUsed, resource.capacity);
        nonRenewableUsage_[req.resource] = newUsed;
    }
}

void ProjectJobSchedulingIncrementalScoreCalculator::setRecordedDates(
    int allocationIndex, int startDate, int endDate)
{
    if (schedule_->jobTypeOf(allocationIndex) == JobType::SINK)
    {
        const int criticalPathEnd
            = schedule_->projectCriticalPathEndDateOf(allocationIndex);
        mediumTotal_ -= std::max(0, end_[allocationIndex] - criticalPathEnd);
        mediumTotal_ += std::max(0, endDate - criticalPathEnd);
    }
    start_[allocationIndex] = startDate;
    end_[allocationIndex] = endDate;
}

int ProjectJobSchedulingIncrementalScoreCalculator::predecessorsDoneDate(
    int allocationIndex) const
{
    int doneDate = 0;
    for (int p : schedule_->allocations[allocationIndex].predecessorAllocations)
    {
        doneDate = std::max(doneDate, end_[p]);
    }
    return doneDate;
}

void ProjectJobSchedulingIncrementalScoreCalculator::afterVariableChanged(
    int allocationIndex)
{
    ProjectJobSchedule &schedule = *schedule_;
    const int newValue = schedule.allocations[allocationIndex].modeDelay;
    const int oldValue = assign_[allocationIndex];
    if (newValue == oldValue)
        return;

    const int oldMode = modeIdx_[allocationIndex];
    const int newMode
        = newValue < 0
              ? -1
              : schedule.executionModeIndexOfValue(allocationIndex, newValue);

    // Non-renewable usage is date-independent: swap the mode contribution.
    if (oldMode != newMode)
    {
        applyNonRenewable(allocationIndex, oldMode, -1);
        applyNonRenewable(allocationIndex, newMode, +1);
    }

    // The changed allocation: retract the old busy window, recompute its
    // dates from the (unchanged) predecessors, insert the new window.
    const int doneDate = predecessorsDoneDate(allocationIndex);
    const int newStart
        = doneDate + (newValue < 0 ? 0 : schedule.delayOfValue(newValue));
    const int newEnd
        = newStart
          + (newMode < 0
                 ? 0
                 : schedule.jobs[schedule.allocations[allocationIndex].job]
                       .executionModes[newMode]
                       .duration);
    const bool windowChanged = newStart != start_[allocationIndex]
                               || newEnd != end_[allocationIndex];
    if (oldMode != newMode || windowChanged)
    {
        applyRenewable(allocationIndex,
                       oldMode,
                       start_[allocationIndex],
                       end_[allocationIndex],
                       -1);
        applyRenewable(allocationIndex, newMode, newStart, newEnd, +1);
    }
    const bool endChanged = newEnd != end_[allocationIndex];
    setRecordedDates(allocationIndex, newStart, newEnd);
    assign_[allocationIndex] = newValue;
    modeIdx_[allocationIndex] = newMode;
    if (!endChanged)
        return;

    // Re-propagate the shadow chain over the successor subgraph, in
    // topological order (min-heap on the topological position, so every
    // node is recomputed after all of its changed predecessors). A node
    // whose end date is unchanged stops the propagation. The heap/touched
    // member buffers avoid a per-move allocation (profiled hotspot).
    std::vector<std::pair<int, int>> &heap = propagationHeap_;
    for (int s : schedule.successorAllocations[allocationIndex])
    {
        heap.emplace_back(topologicalPosition_[s], s);
        std::push_heap(heap.begin(), heap.end(), std::greater<>());
        queued_[s] = 1;
        touched_.push_back(s);
    }
    while (!heap.empty())
    {
        const int node = heap.front().second;
        std::pop_heap(heap.begin(), heap.end(), std::greater<>());
        heap.pop_back();
        const int done = predecessorsDoneDate(node);
        const int value = assign_[node];
        const int start = done + (value < 0 ? 0 : schedule.delayOfValue(value));
        const int end = start
                        + (modeIdx_[node] < 0
                               ? 0
                               : schedule.jobs[schedule.allocations[node].job]
                                     .executionModes[modeIdx_[node]]
                                     .duration);
        if (start == start_[node] && end == end_[node])
            continue;
        applyRenewable(node, modeIdx_[node], start_[node], end_[node], -1);
        applyRenewable(node, modeIdx_[node], start, end, +1);
        const bool nodeEndChanged = end != end_[node];
        setRecordedDates(node, start, end);
        if (!nodeEndChanged)
            continue;
        for (int s : schedule.successorAllocations[node])
        {
            if (queued_[s])
                continue;
            heap.emplace_back(topologicalPosition_[s], s);
            std::push_heap(heap.begin(), heap.end(), std::greater<>());
            queued_[s] = 1;
            touched_.push_back(s);
        }
    }
    for (int t : touched_)
        queued_[t] = 0;
    touched_.clear();
}

optimaru::HardSoftScore
ProjectJobSchedulingIncrementalScoreCalculator::calculateScore()
{
    std::int64_t makespan = 0;
    for (int s : sinkAllocations_)
        makespan = std::max<std::int64_t>(makespan, end_[s]);
    return toEngineScore(
        Score{-(hardNonRenewable_ + hardRenewable_), -mediumTotal_, -makespan});
}

}  // namespace pjs
