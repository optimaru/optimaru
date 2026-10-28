#include "pjs/score_calculator.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pjs
{

void ScoreCalculator::prepare(ProjectJobSchedule &schedule)
{
    if (schedule.prepared)
        return;
    if (schedule.delayRangeSize < 1)
    {
        throw std::invalid_argument("delayRangeSize must be >= 1");
    }
    const int allocationCount = schedule.allocationCount();
    const int jobCount = static_cast<int>(schedule.jobs.size());
    const int projectCount = static_cast<int>(schedule.projects.size());
    const int resourceCount = static_cast<int>(schedule.resources.size());

    int maxModeCount = 1;
    for (int j = 0; j < jobCount; ++j)
    {
        const Job &job = schedule.jobs[j];
        if (job.project < 0 || job.project >= projectCount)
        {
            throw std::invalid_argument("job " + job.id
                                        + ": project index out of range");
        }
        if (job.executionModes.empty())
        {
            throw std::invalid_argument(
                "job " + job.id
                + ": every job needs at least one execution "
                  "mode (Java SOURCE/SINK carry a "
                  "zero-duration mode)");
        }
        maxModeCount = std::max(maxModeCount,
                                static_cast<int>(job.executionModes.size()));
        for (const ExecutionMode &mode : job.executionModes)
        {
            for (const ResourceRequirement &req : mode.resourceRequirements)
            {
                if (req.resource < 0 || req.resource >= resourceCount)
                {
                    throw std::invalid_argument(
                        "execution mode " + mode.id
                        + ": resource index out of range");
                }
            }
        }
    }
    schedule.maxModeCount = maxModeCount;

    // Successor lists (inverse of the predecessor lists) + Kahn topological
    // order over the precedence DAG.
    schedule.successorAllocations.assign(allocationCount, {});
    std::vector<int> indegree(allocationCount, 0);
    for (int a = 0; a < allocationCount; ++a)
    {
        const Allocation &allocation = schedule.allocations[a];
        if (allocation.job < 0 || allocation.job >= jobCount)
        {
            throw std::invalid_argument("allocation " + allocation.id
                                        + ": job index out of range");
        }
        for (int p : allocation.predecessorAllocations)
        {
            if (p < 0 || p >= allocationCount || p == a)
            {
                throw std::invalid_argument(
                    "allocation " + allocation.id
                    + ": predecessor index out of range");
            }
            schedule.successorAllocations[p].push_back(a);
            ++indegree[a];
        }
    }
    schedule.topologicalOrder.clear();
    schedule.topologicalOrder.reserve(allocationCount);
    for (int a = 0; a < allocationCount; ++a)
    {
        if (indegree[a] == 0)
            schedule.topologicalOrder.push_back(a);
    }
    for (std::size_t head = 0; head < schedule.topologicalOrder.size(); ++head)
    {
        const int a = schedule.topologicalOrder[head];
        for (int s : schedule.successorAllocations[a])
        {
            if (--indegree[s] == 0)
                schedule.topologicalOrder.push_back(s);
        }
    }
    if (static_cast<int>(schedule.topologicalOrder.size()) != allocationCount)
    {
        throw std::invalid_argument(
            "the allocation precedence graph contains a cycle");
    }
    schedule.prepared = true;
}

void ScoreCalculator::updateDates(ProjectJobSchedule &schedule)
{
    for (int a : schedule.topologicalOrder)
    {
        Allocation &allocation = schedule.allocations[a];
        int doneDate = 0;
        for (int p : allocation.predecessorAllocations)
        {
            doneDate = std::max(doneDate, schedule.allocations[p].endDate);
        }
        const int value = allocation.modeDelay;
        const int delay = value < 0 ? 0 : schedule.delayOfValue(value);
        const int duration
            = value < 0 ? 0 : schedule.executionModeOf(a, value).duration;
        allocation.predecessorsDoneDate = doneDate;
        allocation.startDate = doneDate + delay;
        allocation.endDate = allocation.startDate + duration;
    }
}

Score ScoreCalculator::calculateScore(ProjectJobSchedule &schedule)
{
    prepare(schedule);
    updateDates(schedule);

    const int resourceCount = static_cast<int>(schedule.resources.size());

    // hard 1: non-renewable — total requirement over the whole horizon.
    std::vector<std::int64_t> nonRenewableUsage(resourceCount, 0);
    // hard 2: renewable — requirement per (resource, day).
    std::vector<std::vector<int>> renewableUsage(resourceCount);

    for (int a = 0; a < schedule.allocationCount(); ++a)
    {
        const Allocation &allocation = schedule.allocations[a];
        if (allocation.modeDelay < 0)
            continue;  // unassigned: no resource usage
        const ExecutionMode &mode
            = schedule.executionModeOf(a, allocation.modeDelay);
        for (const ResourceRequirement &req : mode.resourceRequirements)
        {
            const Resource &resource = schedule.resources[req.resource];
            if (!resource.renewable)
            {
                nonRenewableUsage[req.resource] += req.requirement;
            }
            else if (allocation.endDate > allocation.startDate)
            {
                std::vector<int> &usage = renewableUsage[req.resource];
                if (static_cast<int>(usage.size()) < allocation.endDate)
                    usage.resize(allocation.endDate, 0);
                for (int d = allocation.startDate; d < allocation.endDate; ++d)
                    usage[d] += req.requirement;
            }
        }
    }

    std::int64_t hard = 0;
    for (int r = 0; r < resourceCount; ++r)
    {
        const Resource &resource = schedule.resources[r];
        if (!resource.renewable)
        {
            if (nonRenewableUsage[r] > resource.capacity)
                hard += nonRenewableUsage[r] - resource.capacity;
        }
        else
        {
            for (int used : renewableUsage[r])
            {
                if (used > resource.capacity)
                    hard += used - resource.capacity;
            }
        }
    }

    // medium 3: total project delay; soft 4: total makespan (max sink end).
    std::int64_t medium = 0;
    std::int64_t makespan = 0;
    bool hasSink = false;
    for (int a = 0; a < schedule.allocationCount(); ++a)
    {
        if (schedule.jobTypeOf(a) != JobType::SINK)
            continue;
        const int endDate = schedule.allocations[a].endDate;
        const int projectDelay
            = endDate - schedule.projectCriticalPathEndDateOf(a);
        if (projectDelay > 0)
            medium += projectDelay;
        makespan
            = hasSink ? std::max<std::int64_t>(makespan, endDate) : endDate;
        hasSink = true;
    }

    schedule.score = Score{-hard, -medium, -makespan};
    return schedule.score;
}

}  // namespace pjs
