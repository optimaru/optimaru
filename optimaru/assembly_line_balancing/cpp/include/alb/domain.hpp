#ifndef ALB_DOMAIN_HPP
#define ALB_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace alb
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// Simple assembly-line balancing (SALBP-1 with zoning and station domains)
// scores with Score, exactly the engine's score type, so the mapping
// is the identity and there is no medium level (no optimaru::MediumFoldOf
// specialization). At the components' default weights (alb/constraints/*.hpp):
//   hard = -(station capacity overflow + reversed precedence arcs
//            + stations beyond max_stations + station-domain distance
//            + missed fixed stations + forbidden stations hit
//            + extra stations of same-station groups
//            + different-station pairs sharing a station)
//   soft = -(stations used + 0 * workload range)
// Every raw cost is an integer; soft is stored in a double, exact for these
// magnitudes (< 2^53).
using Score = optimaru::Score;

// ---------------------------------------------------------------------------
// Task  (entity)
// ---------------------------------------------------------------------------
// `station` is the single BASIC decision variable: a station index
// (0-based), -1 == unassigned. Stations are not bounded above by the facts:
// the value range is AssemblyLineBalancingProblem::stationCount (see there).
struct Task
{
    std::string id;
    std::int64_t duration = 0;  // > 0 (checked by ScoreCalculator::prepare)
    int minStation = 0;
    std::optional<int> maxStation;    // none = no upper bound
    std::optional<int> fixedStation;  // none = not fixed (a penalty, not a pin)
    // Kept as given (duplicates too, so from_core round-trips); the scoring
    // reads the sorted unique copy built by prepare().
    std::vector<int> forbiddenStations;

    int station = -1;  // decision variable; -1 == unassigned

    bool isAssigned() const { return station >= 0; }
};

// `before` must not sit on a later station than `after` (task indices; a
// duplicate arc counts once per copy, a self-arc never costs anything but
// makes the construction refuse the graph as cyclic).
struct Precedence
{
    int before = 0;
    int after = 0;
};

// Tasks that should share one station (indices as given; a task listed twice
// counts once).
struct TaskGroup
{
    std::string id;
    std::vector<int> tasks;
};

// Two tasks that must not share a station (first == second is allowed and
// then always costs 1 while the task is assigned).
struct TaskPair
{
    int first = 0;
    int second = 0;
};

// ---------------------------------------------------------------------------
// AssemblyLineBalancingProblem  (solution)
// ---------------------------------------------------------------------------
struct AssemblyLineBalancingProblem
{
    std::int64_t cycleTime = 0;  // > 0: the capacity of every station
    std::vector<Task> tasks;
    std::vector<Precedence> precedences;
    std::optional<int> maxStations;  // none = no limit
    std::vector<TaskGroup> sameStationGroups;
    std::vector<TaskPair> differentStationPairs;

    // Mirrors the model's warm_start: when false the Solver clears every
    // station before its construction (a plain solve ignores input stations);
    // when true the assigned stations are the seed. The Solver resets it to
    // false after the solve, so a seed is used once.
    bool warmStart = false;

    // The BASIC value range V: the engine draws stations from [0, V).
    // V = max(task count, 1 + max station), refreshed by
    // ScoreCalculator::prepare() — which the Solver calls once per solve,
    // AFTER its construction (which may place tasks beyond the task count) and
    // before the engine starts. It stays frozen during the search: the
    // decomposed calculator sizes its per-station caches from it at every
    // reset. V >= task count is exactly CP-SAT's search space.
    int stationCount = 0;

    Score score;

    // --- derived lookups, rebuilt by ScoreCalculator::prepare() -------------
    bool prepared = false;
    std::vector<std::vector<int>> arcsOut;  // per task: arcs it is `before` of (no self-arcs)
    std::vector<std::vector<int>> arcsIn;   // per task: arcs it is `after` of (no self-arcs)
    std::vector<std::vector<int>> groupMembers;  // per group: distinct members, first-seen order
    std::vector<std::vector<int>> groupsOf;      // per task: the groups it belongs to (once each)
    std::vector<std::vector<int>> pairsOf;  // per task: its pairs (a self-pair once)
    std::vector<std::vector<int>> forbiddenSorted;  // per task: sorted, unique

    int taskCount() const { return static_cast<int>(tasks.size()); }

    // Processing time per station 0..max(station) (empty middle stations
    // listed as 0); unassigned tasks carry no load. The model's
    // station_loads output.
    std::vector<std::int64_t> stationLoads() const
    {
        int top = -1;
        for (const Task &task : tasks)
            top = task.station > top ? task.station : top;
        std::vector<std::int64_t> loads(static_cast<std::size_t>(top + 1), 0);
        for (const Task &task : tasks)
        {
            if (task.isAssigned())
                loads[task.station] += task.duration;
        }
        return loads;
    }

    // Stations with a load > 0 (durations are > 0, so: non-empty stations).
    int stationsUsed() const
    {
        int used = 0;
        for (std::int64_t load : stationLoads())
            used += load > 0 ? 1 : 0;
        return used;
    }

    // The current station per task (-1 == unassigned).
    std::vector<int> assignment() const
    {
        std::vector<int> stations;
        stations.reserve(tasks.size());
        for (const Task &task : tasks)
            stations.push_back(task.station);
        return stations;
    }
};

}  // namespace alb

// ---------------------------------------------------------------------------
// SolutionTraits specialization: entities are tasks, the decision
// variable is Task::station, the value range is the frozen station count.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<alb::AssemblyLineBalancingProblem>
{
    static constexpr optimaru::Shape shape
        = optimaru::Shape::Assignment;
    static int entity_count(const alb::AssemblyLineBalancingProblem &problem)
    {
        return problem.taskCount();
    }
    static int value_count(const alb::AssemblyLineBalancingProblem &problem)
    {
        // Never a live max(station): a calculator sizes its per-station
        // caches from this at reset, so it must not change mid-search.
        if (!problem.prepared)
        {
            throw std::logic_error("alb: ScoreCalculator::prepare(problem) "
                                   "must run before scoring or solving");
        }
        return problem.stationCount;
    }
    static int value_of(const alb::AssemblyLineBalancingProblem &problem,
                           int taskIndex)
    {
        return problem.tasks[taskIndex].station;
    }
    static void assign(alb::AssemblyLineBalancingProblem &problem,
                            int taskIndex,
                            int station)
    {
        problem.tasks[taskIndex].station = station;
    }
};

#endif  // ALB_DOMAIN_HPP
