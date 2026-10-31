#include "alb/incremental_calculator.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
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
#include "alb/score_calculator.hpp"

namespace alb
{

namespace
{

// The same constants the components and ScoreCalculator read (integral; see
// score_calculator.cpp, which also checks the levels).
template <typename Component>
constexpr std::int64_t weightOf()
{
    return static_cast<std::int64_t>(Component::kDefaultWeight);
}

// workloadImbalanceCost is an opt-in objective (default weight 0): the
// monolithic path tracks its multiset only when that default is non-zero.
constexpr bool kTrackWorkload = weightOf<WorkloadImbalanceCost>() != 0;

}  // namespace

void AssemblyLineBalancingIncrementalScoreCalculator::resetWorkingSolution(
    AssemblyLineBalancingProblem &workingSolution)
{
    if (!workingSolution.prepared)
    {
        throw std::logic_error("alb incremental calculator requires "
                               "ScoreCalculator::prepare(problem) first");
    }
    problem_ = &workingSolution;
    const int n = workingSolution.taskCount();
    station_.assign(n, -1);
    load_.assign(workingSolution.stationCount, 0);
    count_.assign(workingSolution.stationCount, 0);
    groupTally_.assign(workingSolution.groupMembers.size(), {});
    groupDistinct_.assign(workingSolution.groupMembers.size(), 0);
    usedLoads_.clear();
    overCapacity_ = reversedArcs_ = beyondMaxStations_ = outsideBounds_ = 0;
    missedFixed_ = forbiddenHits_ = groupExtra_ = pairsTogether_ = usedStations_ = 0;
    for (int t = 0; t < n; ++t)
    {
        const int station = workingSolution.tasks[t].station;
        if (station >= 0)
            insert(t, station);
    }
}

void AssemblyLineBalancingIncrementalScoreCalculator::ensureStation(int station)
{
    // The engine draws stations below stationCount; this only guards a
    // caller that wrote a larger one without re-running prepare().
    if (station >= static_cast<int>(load_.size()))
    {
        load_.resize(station + 1, 0);
        count_.resize(station + 1, 0);
    }
}

std::int64_t AssemblyLineBalancingIncrementalScoreCalculator::overflowOf(int station) const
{
    const std::int64_t over = load_[station] - problem_->cycleTime;
    return over > 0 ? over : 0;
}

void AssemblyLineBalancingIncrementalScoreCalculator::moveLoad(int station,
                                                               std::int64_t delta,
                                                               int countDelta)
{
    overCapacity_ -= overflowOf(station);
    if (kTrackWorkload && load_[station] > 0)
        usedLoads_.erase(usedLoads_.find(load_[station]));
    load_[station] += delta;
    if (kTrackWorkload && load_[station] > 0)
        usedLoads_.insert(load_[station]);
    overCapacity_ += overflowOf(station);

    const int before = count_[station];
    count_[station] += countDelta;
    if (before == 0 && count_[station] == 1)
        ++usedStations_;
    else if (before == 1 && count_[station] == 0)
        --usedStations_;
}

void AssemblyLineBalancingIncrementalScoreCalculator::addTaskTerms(int task,
                                                                   int station,
                                                                   int sign)
{
    const AssemblyLineBalancingProblem &p = *problem_;
    const Task &t = p.tasks[task];
    if (p.maxStations)
        beyondMaxStations_ += sign * std::max(0, station - *p.maxStations + 1);
    int outside = std::max(0, t.minStation - station);
    if (t.maxStation)
        outside += std::max(0, station - *t.maxStation);
    outsideBounds_ += sign * outside;
    if (t.fixedStation && *t.fixedStation != station)
        missedFixed_ += sign;
    const std::vector<int> &forbidden = p.forbiddenSorted[task];
    if (std::binary_search(forbidden.begin(), forbidden.end(), station))
        forbiddenHits_ += sign;
}

std::int64_t AssemblyLineBalancingIncrementalScoreCalculator::reversedArcsOf(int task,
                                                                             int station) const
{
    const AssemblyLineBalancingProblem &p = *problem_;
    std::int64_t count = 0;
    for (int arc : p.arcsOut[task])
    {
        const int after = station_[p.precedences[arc].after];
        if (after >= 0 && station > after)
            ++count;
    }
    for (int arc : p.arcsIn[task])
    {
        const int before = station_[p.precedences[arc].before];
        if (before >= 0 && before > station)
            ++count;
    }
    return count;
}

std::int64_t AssemblyLineBalancingIncrementalScoreCalculator::pairsWith(int task,
                                                                        int station) const
{
    const AssemblyLineBalancingProblem &p = *problem_;
    std::int64_t together = 0;
    for (int index : p.pairsOf[task])
    {
        const TaskPair &pair = p.differentStationPairs[index];
        if (pair.first == pair.second)
        {
            ++together;
            continue;
        }
        const int partner = pair.first == task ? pair.second : pair.first;
        if (station_[partner] == station)
            ++together;
    }
    return together;
}

void AssemblyLineBalancingIncrementalScoreCalculator::updateGroups(int task,
                                                                   int station,
                                                                   bool joined)
{
    for (int group : problem_->groupsOf[task])
    {
        std::vector<std::pair<int, int>> &tally = groupTally_[group];
        std::size_t at = 0;
        while (at < tally.size() && tally[at].first != station)
            ++at;
        const int before = groupDistinct_[group];
        if (joined)
        {
            if (at == tally.size())
            {
                tally.emplace_back(station, 1);
                ++groupDistinct_[group];
            }
            else
            {
                ++tally[at].second;
            }
        }
        else if (--tally[at].second == 0)
        {
            tally[at] = tally.back();
            tally.pop_back();
            --groupDistinct_[group];
        }
        groupExtra_ += std::max(0, groupDistinct_[group] - 1) - std::max(0, before - 1);
    }
}

void AssemblyLineBalancingIncrementalScoreCalculator::insert(int task, int station)
{
    ensureStation(station);
    station_[task] = station;
    moveLoad(station, problem_->tasks[task].duration, +1);
    addTaskTerms(task, station, +1);
    reversedArcs_ += reversedArcsOf(task, station);
    pairsTogether_ += pairsWith(task, station);
    updateGroups(task, station, true);
}

// Exact mirror of insert.
void AssemblyLineBalancingIncrementalScoreCalculator::retract(int task, int station)
{
    updateGroups(task, station, false);
    pairsTogether_ -= pairsWith(task, station);
    reversedArcs_ -= reversedArcsOf(task, station);
    addTaskTerms(task, station, -1);
    moveLoad(station, -problem_->tasks[task].duration, -1);
    station_[task] = -1;
}

void AssemblyLineBalancingIncrementalScoreCalculator::beforeVariableChanged(int taskIndex)
{
    const int old = station_[taskIndex];
    if (old >= 0)
        retract(taskIndex, old);  // unassigned: nothing to retract
}

void AssemblyLineBalancingIncrementalScoreCalculator::afterVariableChanged(int taskIndex)
{
    const int now = problem_->tasks[taskIndex].station;  // the new value
    if (now >= 0)
        insert(taskIndex, now);  // now unassigned: nothing to insert
}

RawCosts AssemblyLineBalancingIncrementalScoreCalculator::rawCosts() const
{
    RawCosts raw;
    raw.stationCapacityCost = overCapacity_;
    raw.taskPrecedenceCost = reversedArcs_;
    raw.maxStationsCost = beyondMaxStations_;
    raw.taskStationBoundsCost = outsideBounds_;
    raw.fixedTaskStationsCost = missedFixed_;
    raw.forbiddenTaskStationsCost = forbiddenHits_;
    raw.sameStationGroupsCost = groupExtra_;
    raw.differentStationPairsCost = pairsTogether_;
    raw.usedStationsCost = usedStations_;
    if (kTrackWorkload)
    {
        if (!usedLoads_.empty())
            raw.workloadImbalanceCost = *usedLoads_.rbegin() - *usedLoads_.begin();
    }
    else
    {
        std::int64_t heaviest = 0;
        std::int64_t lightest = 0;
        bool any = false;
        for (std::int64_t load : load_)
        {
            if (load <= 0)
                continue;
            heaviest = any ? std::max(heaviest, load) : load;
            lightest = any ? std::min(lightest, load) : load;
            any = true;
        }
        raw.workloadImbalanceCost = heaviest - lightest;
    }
    return raw;
}

Score AssemblyLineBalancingIncrementalScoreCalculator::calculateScore()
{
    std::int64_t workload = 0;
    if (kTrackWorkload && !usedLoads_.empty())
        workload = *usedLoads_.rbegin() - *usedLoads_.begin();
    Score score;
    score.hard = -(weightOf<StationCapacityCost>() * overCapacity_
                   + weightOf<TaskPrecedenceCost>() * reversedArcs_
                   + weightOf<MaxStationsCost>() * beyondMaxStations_
                   + weightOf<TaskStationBoundsCost>() * outsideBounds_
                   + weightOf<FixedTaskStationsCost>() * missedFixed_
                   + weightOf<ForbiddenTaskStationsCost>() * forbiddenHits_
                   + weightOf<SameStationGroupsCost>() * groupExtra_
                   + weightOf<DifferentStationPairsCost>() * pairsTogether_);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(-(weightOf<UsedStationsCost>() * usedStations_
                                       + weightOf<WorkloadImbalanceCost>() * workload));
    return score;
}

}  // namespace alb
