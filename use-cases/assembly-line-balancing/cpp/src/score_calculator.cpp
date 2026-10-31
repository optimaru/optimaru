#include "alb/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "alb/constraints/different_station_pairs_cost.hpp"
#include "alb/constraints/fixed_task_stations_cost.hpp"
#include "alb/constraints/forbidden_task_stations_cost.hpp"
#include "alb/constraints/max_stations_cost.hpp"
#include "alb/constraints/same_station_groups_cost.hpp"
#include "alb/constraints/station_capacity_cost.hpp"
#include "alb/constraints/task_precedence_cost.hpp"
#include "alb/constraints/task_station_bounds_cost.hpp"
#include "alb/constraints/used_stations_cost.hpp"
#include "alb/constraints/workload_imbalance_cost.hpp"

namespace alb
{

namespace
{

// The monolithic score is the default assembly, so it reads the same
// constants the components carry. Integral, so both levels are computed in
// int64.
template <typename Component>
constexpr std::int64_t weightOf()
{
    return static_cast<std::int64_t>(Component::kDefaultWeight);
}

static_assert(weightOf<StationCapacityCost>() == StationCapacityCost::kDefaultWeight
                  && weightOf<TaskPrecedenceCost>() == TaskPrecedenceCost::kDefaultWeight
                  && weightOf<MaxStationsCost>() == MaxStationsCost::kDefaultWeight
                  && weightOf<TaskStationBoundsCost>() == TaskStationBoundsCost::kDefaultWeight
                  && weightOf<FixedTaskStationsCost>() == FixedTaskStationsCost::kDefaultWeight
                  && weightOf<ForbiddenTaskStationsCost>()
                         == ForbiddenTaskStationsCost::kDefaultWeight
                  && weightOf<SameStationGroupsCost>() == SameStationGroupsCost::kDefaultWeight
                  && weightOf<DifferentStationPairsCost>()
                         == DifferentStationPairsCost::kDefaultWeight
                  && weightOf<UsedStationsCost>() == UsedStationsCost::kDefaultWeight
                  && weightOf<WorkloadImbalanceCost>() == WorkloadImbalanceCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(StationCapacityCost::kDefaultLevel == optimaru::Level::Hard
                  && TaskPrecedenceCost::kDefaultLevel == optimaru::Level::Hard
                  && MaxStationsCost::kDefaultLevel == optimaru::Level::Hard
                  && TaskStationBoundsCost::kDefaultLevel == optimaru::Level::Hard
                  && FixedTaskStationsCost::kDefaultLevel == optimaru::Level::Hard
                  && ForbiddenTaskStationsCost::kDefaultLevel == optimaru::Level::Hard
                  && SameStationGroupsCost::kDefaultLevel == optimaru::Level::Hard
                  && DifferentStationPairsCost::kDefaultLevel == optimaru::Level::Hard
                  && UsedStationsCost::kDefaultLevel == optimaru::Level::Soft
                  && WorkloadImbalanceCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes these default levels");

void requireIndex(int index, int count, const char *what)
{
    if (index < 0 || index >= count)
    {
        throw std::invalid_argument(std::string(what) + " index " + std::to_string(index)
                                    + " is out of range 0.." + std::to_string(count - 1));
    }
}

}  // namespace

void ScoreCalculator::prepare(AssemblyLineBalancingProblem &problem)
{
    const int n = problem.taskCount();
    if (problem.cycleTime <= 0)
        throw std::invalid_argument("cycle_time must be positive");
    int top = -1;
    for (const Task &task : problem.tasks)
    {
        if (task.duration <= 0)
            throw std::invalid_argument("task '" + task.id + "': duration must be positive");
        if (task.station < -1)
        {
            throw std::invalid_argument("task '" + task.id + "': station "
                                        + std::to_string(task.station) + " is below -1");
        }
        top = std::max(top, task.station);
    }

    problem.arcsOut.assign(n, {});
    problem.arcsIn.assign(n, {});
    for (std::size_t a = 0; a < problem.precedences.size(); ++a)
    {
        const Precedence &arc = problem.precedences[a];
        requireIndex(arc.before, n, "precedence task");
        requireIndex(arc.after, n, "precedence task");
        if (arc.before == arc.after)
            continue;  // a self-arc never costs anything
        problem.arcsOut[arc.before].push_back(static_cast<int>(a));
        problem.arcsIn[arc.after].push_back(static_cast<int>(a));
    }

    problem.groupMembers.assign(problem.sameStationGroups.size(), {});
    problem.groupsOf.assign(n, {});
    for (std::size_t g = 0; g < problem.sameStationGroups.size(); ++g)
    {
        std::vector<int> &members = problem.groupMembers[g];
        for (int task : problem.sameStationGroups[g].tasks)
        {
            requireIndex(task, n, "group task");
            if (std::find(members.begin(), members.end(), task) == members.end())
            {
                members.push_back(task);
                problem.groupsOf[task].push_back(static_cast<int>(g));
            }
        }
    }

    problem.pairsOf.assign(n, {});
    for (std::size_t p = 0; p < problem.differentStationPairs.size(); ++p)
    {
        const TaskPair &pair = problem.differentStationPairs[p];
        requireIndex(pair.first, n, "pair task");
        requireIndex(pair.second, n, "pair task");
        problem.pairsOf[pair.first].push_back(static_cast<int>(p));
        if (pair.second != pair.first)
            problem.pairsOf[pair.second].push_back(static_cast<int>(p));
    }

    problem.forbiddenSorted.assign(n, {});
    for (int t = 0; t < n; ++t)
    {
        std::vector<int> &forbidden = problem.forbiddenSorted[t];
        forbidden = problem.tasks[t].forbiddenStations;
        std::sort(forbidden.begin(), forbidden.end());
        forbidden.erase(std::unique(forbidden.begin(), forbidden.end()), forbidden.end());
    }

    problem.stationCount = std::max(n, top + 1);
    problem.prepared = true;
}

RawCosts ScoreCalculator::rawCosts(AssemblyLineBalancingProblem &problem)
{
    if (!problem.prepared)
        prepare(problem);

    RawCosts raw;
    int top = -1;
    for (const Task &task : problem.tasks)
        top = std::max(top, task.station);
    std::vector<std::int64_t> load(static_cast<std::size_t>(top + 1), 0);

    for (int t = 0; t < problem.taskCount(); ++t)
    {
        const Task &task = problem.tasks[t];
        const int s = task.station;
        if (s < 0)
            continue;  // unassigned: contributes nothing
        load[s] += task.duration;
        if (problem.maxStations)
            raw.maxStationsCost += std::max(0, s - *problem.maxStations + 1);
        raw.taskStationBoundsCost += std::max(0, task.minStation - s);
        if (task.maxStation)
            raw.taskStationBoundsCost += std::max(0, s - *task.maxStation);
        if (task.fixedStation && *task.fixedStation != s)
            ++raw.fixedTaskStationsCost;
        const std::vector<int> &forbidden = problem.forbiddenSorted[t];
        if (std::binary_search(forbidden.begin(), forbidden.end(), s))
            ++raw.forbiddenTaskStationsCost;
    }

    std::int64_t heaviest = 0;
    std::int64_t lightest = 0;
    for (std::int64_t stationLoad : load)
    {
        raw.stationCapacityCost += std::max<std::int64_t>(0, stationLoad - problem.cycleTime);
        if (stationLoad > 0)
        {
            heaviest = raw.usedStationsCost == 0 ? stationLoad : std::max(heaviest, stationLoad);
            lightest = raw.usedStationsCost == 0 ? stationLoad : std::min(lightest, stationLoad);
            ++raw.usedStationsCost;
        }
    }
    raw.workloadImbalanceCost = heaviest - lightest;

    for (const Precedence &arc : problem.precedences)
    {
        const int before = problem.tasks[arc.before].station;
        const int after = problem.tasks[arc.after].station;
        if (before >= 0 && after >= 0 && before > after)
            ++raw.taskPrecedenceCost;
    }

    for (const std::vector<int> &members : problem.groupMembers)
    {
        std::set<int> stations;
        for (int task : members)
        {
            if (problem.tasks[task].station >= 0)
                stations.insert(problem.tasks[task].station);
        }
        if (stations.size() > 1)
            raw.sameStationGroupsCost += static_cast<std::int64_t>(stations.size()) - 1;
    }

    for (const TaskPair &pair : problem.differentStationPairs)
    {
        const int first = problem.tasks[pair.first].station;
        if (first >= 0 && first == problem.tasks[pair.second].station)
            ++raw.differentStationPairsCost;
    }
    return raw;
}

Score ScoreCalculator::calculateScore(AssemblyLineBalancingProblem &problem)
{
    const RawCosts raw = rawCosts(problem);
    Score score;
    score.hard = -(weightOf<StationCapacityCost>() * raw.stationCapacityCost
                   + weightOf<TaskPrecedenceCost>() * raw.taskPrecedenceCost
                   + weightOf<MaxStationsCost>() * raw.maxStationsCost
                   + weightOf<TaskStationBoundsCost>() * raw.taskStationBoundsCost
                   + weightOf<FixedTaskStationsCost>() * raw.fixedTaskStationsCost
                   + weightOf<ForbiddenTaskStationsCost>() * raw.forbiddenTaskStationsCost
                   + weightOf<SameStationGroupsCost>() * raw.sameStationGroupsCost
                   + weightOf<DifferentStationPairsCost>() * raw.differentStationPairsCost);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(
        -(weightOf<UsedStationsCost>() * raw.usedStationsCost
          + weightOf<WorkloadImbalanceCost>() * raw.workloadImbalanceCost));
    problem.score = score;
    return score;
}

}  // namespace alb
