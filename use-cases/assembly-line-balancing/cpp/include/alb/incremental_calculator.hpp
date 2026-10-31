#ifndef ALB_INCREMENTAL_CALCULATOR_HPP
#define ALB_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "alb/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace alb
{

// The assembly-line-balancing implementation of the engine's
// optimaru::IncrementalScoreCalculator: the default constraint set at the
// components' kDefaultWeight, hand-fused into one set of aggregates.
//
// A task change touches only its old and new station and its own arcs,
// groups and pairs:
//   - stationCapacityCost: per-station loads; the station's overflow term is
//     replaced (O(1)).
//   - taskPrecedenceCost / differentStationPairsCost: the arcs / pairs of the
//     task, evaluated against the other endpoint's mirrored station
//     (O(degree)).
//   - maxStationsCost, taskStationBoundsCost, fixedTaskStationsCost,
//     forbiddenTaskStationsCost: per-task terms of its station (O(1), the
//     forbidden lookup a binary search).
//   - sameStationGroupsCost: a station -> member-count tally per group.
//   - usedStationsCost: member counts per station (0 <-> 1 transitions).
//   - workloadImbalanceCost: default weight 0, so it is maintained (an
//     ordered multiset of the non-zero loads) only if that default changes.
//
// beforeVariableChanged retracts the task while `station` still holds the OLD
// value; afterVariableChanged inserts the NEW value. Unassigned tasks
// (station == -1) contribute nothing. The full-recompute reference is
// ScoreCalculator::calculateScore (checked in the tests and under
// EnvironmentMode::FULL_ASSERT). Requires ScoreCalculator::prepare(problem).
class AssemblyLineBalancingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<AssemblyLineBalancingProblem>
{
public:
    void resetWorkingSolution(AssemblyLineBalancingProblem &workingSolution) override;
    void beforeVariableChanged(int taskIndex) override;
    void afterVariableChanged(int taskIndex) override;
    Score calculateScore() override;

private:
    AssemblyLineBalancingProblem *problem_ = nullptr;
    std::vector<int> station_;  // mirror: station per task (-1 == unassigned)

    // Per-station aggregates.
    std::vector<std::int64_t> load_;
    std::vector<int> count_;
    // Per-group station tallies (station, members there) and distinct counts.
    std::vector<std::vector<std::pair<int, int>>> groupTally_;
    std::vector<int> groupDistinct_;
    std::multiset<std::int64_t> usedLoads_;  // only while workload is weighted

    // Raw totals (positive; weights applied in calculateScore).
    std::int64_t overCapacity_ = 0;
    std::int64_t reversedArcs_ = 0;
    std::int64_t beyondMaxStations_ = 0;
    std::int64_t outsideBounds_ = 0;
    std::int64_t missedFixed_ = 0;
    std::int64_t forbiddenHits_ = 0;
    std::int64_t groupExtra_ = 0;
    std::int64_t pairsTogether_ = 0;
    std::int64_t usedStations_ = 0;

    void insert(int task, int station);
    void retract(int task, int station);
    // The load of `station` moves by `delta` (capacity, used stations,
    // workload multiset).
    void moveLoad(int station, std::int64_t delta, int countDelta);
    // Per-task terms (max stations, bounds, fixed, forbidden) of `task` on
    // `station`, added with `sign`.
    void addTaskTerms(int task, int station, int sign);
    std::int64_t reversedArcsOf(int task, int station) const;
    std::int64_t pairsWith(int task, int station) const;
    void updateGroups(int task, int station, bool joined);
    void ensureStation(int station);
    std::int64_t overflowOf(int station) const;
};

}  // namespace alb

#endif  // ALB_INCREMENTAL_CALCULATOR_HPP
