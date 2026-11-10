#ifndef ALB_CONSTRAINTS_DIFFERENT_STATION_PAIRS_COST_HPP
#define ALB_CONSTRAINTS_DIFFERENT_STATION_PAIRS_COST_HPP

#include <vector>

#include "alb/constraints/common.hpp"

namespace alb {

// Hard, weight 1: differentStationPairsCost — different-station pairs whose
// two tasks share a station. Raw cost per station = the number of pairs with
// both tasks on it (a pair listed twice counts twice; a self-pair costs 1
// while its task is assigned); 0 = every pair separated.
//
// Pairwise delta: a task joining or leaving a station changes the cost by the
// pairs it forms with a partner on that station (plus its self-pairs) — exact
// under the delta contract, O(pairs of the task).
struct DifferentStationPairsCost final : AlbUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DifferentStationPairsCost(double weight = kDefaultWeight)
        : AlbUnitConstraint("differentStationPairsCost", weight, kDefaultLevel) {}
    void on_reset(const AssemblyLineBalancingProblem& problem) override {
        detail::requirePrepared(problem);
    }
    double cost(const AssemblyLineBalancingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        int together = 0;
        for (int task : members) {
            for (int index : problem.pairsOf[task]) {
                const TaskPair& pair = problem.differentStationPairs[index];
                if (pair.first == pair.second)
                    ++together;  // listed once in pairsOf
                else if (pair.first == task && problem.tasks[pair.second].station == unit)
                    ++together;  // counted from its first task only
            }
        }
        return static_cast<double>(together);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const AssemblyLineBalancingProblem& problem, int unit,
                      const std::vector<int>& /*members*/, int entity) const override {
        int together = 0;
        for (int index : problem.pairsOf[entity]) {
            const TaskPair& pair = problem.differentStationPairs[index];
            if (pair.first == pair.second) {
                ++together;
                continue;
            }
            const int partner = pair.first == entity ? pair.second : pair.first;
            if (problem.tasks[partner].station == unit) ++together;
        }
        return static_cast<double>(together);
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_DIFFERENT_STATION_PAIRS_COST_HPP
