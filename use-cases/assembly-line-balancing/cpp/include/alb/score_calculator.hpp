#ifndef ALB_SCORE_CALCULATOR_HPP
#define ALB_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "alb/domain.hpp"

namespace alb
{

// The raw total of every default constraint (before weight, exponent and
// level), named after the registry names of alb/constraints/*.hpp. All of
// them are integers; 0 = no violation.
struct RawCosts
{
    std::int64_t stationCapacityCost = 0;
    std::int64_t taskPrecedenceCost = 0;
    std::int64_t maxStationsCost = 0;
    std::int64_t taskStationBoundsCost = 0;
    std::int64_t fixedTaskStationsCost = 0;
    std::int64_t forbiddenTaskStationsCost = 0;
    std::int64_t sameStationGroupsCost = 0;
    std::int64_t differentStationPairsCost = 0;
    std::int64_t usedStationsCost = 0;
    std::int64_t workloadImbalanceCost = 0;
};

// Monolithic full recompute — the reference the incremental calculator and
// the plugin assembly are checked against (in the tests and under
// FULL_ASSERT).
//
// Constraints (HardSoftScore), the default assembly of alb/constraints.hpp at
// the components' kDefaultWeight:
//   hard 1  stationCapacityCost        per station, max(0, load - cycle time)
//   hard 1  taskPrecedenceCost         arcs (b, a) with station(b) > station(a)
//   hard 1  maxStationsCost            per task, max(0, station - max_stations + 1)
//   hard 1  taskStationBoundsCost      per task, distance below min / above max station
//   hard 1  fixedTaskStationsCost      per task, 1 when off its fixed station
//   hard 1  forbiddenTaskStationsCost  per task, 1 when on a forbidden station
//   hard 1  sameStationGroupsCost      per group, max(0, distinct stations - 1)
//   hard 1  differentStationPairsCost  per pair, 1 when both share a station
//   soft 1  usedStationsCost           stations with a load > 0
//   soft 0  workloadImbalanceCost      max - min load over the used stations
// Unassigned tasks (station == -1) contribute nothing — the engine
// convention: an arc, pair or group member counts only while its tasks are
// assigned.
class ScoreCalculator
{
public:
    // Full recompute at the default weights; stores problem.score. Runs
    // prepare() first if the problem has never been prepared.
    static Score calculateScore(AssemblyLineBalancingProblem &problem);

    // The raw total of every default constraint (prepare() first if needed).
    static RawCosts rawCosts(AssemblyLineBalancingProblem &problem);

    // Validates the facts and the stations and (re)builds the derived lookups
    // (adjacency per task, distinct group members, pairs per task, sorted
    // forbidden stations), then refreshes stationCount = max(task count,
    // 1 + max station). Throws std::invalid_argument on a cycle time or
    // duration <= 0, a station < -1 or an index out of range. Call it after
    // editing the problem and outside a running search (the Solver, the
    // Python calculate_score / constraint_costs do); never during one, since
    // the station count must stay frozen while the engine runs.
    static void prepare(AssemblyLineBalancingProblem &problem);
};

}  // namespace alb

#endif  // ALB_SCORE_CALCULATOR_HPP
