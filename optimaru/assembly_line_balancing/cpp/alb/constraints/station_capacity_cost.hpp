#ifndef ALB_CONSTRAINTS_STATION_CAPACITY_COST_HPP
#define ALB_CONSTRAINTS_STATION_CAPACITY_COST_HPP

#include <cstdint>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb {

// Hard, weight 1: stationCapacityCost — processing time a station carries
// beyond the cycle time. Raw cost per station = max(0, summed duration of its
// tasks - cycle time), so 0 = the station finishes within the cycle.
//
// No delta contract: max(0, load - cycle) is not a sum of per-member terms
// (the clamp makes it non-additive), so the marginal cost of one task depends
// on the whole station. cost() is O(members) either way.
struct StationCapacityCost final : AlbUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit StationCapacityCost(double weight = kDefaultWeight)
        : AlbUnitConstraint("stationCapacityCost", weight, kDefaultLevel) {}
    void on_reset(const AssemblyLineBalancingProblem& problem) override {
        detail::requirePrepared(problem);
    }
    double cost(const AssemblyLineBalancingProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t load = 0;
        for (int task : members) load += problem.tasks[task].duration;
        const std::int64_t over = load - problem.cycleTime;
        return over > 0 ? static_cast<double>(over) : 0.0;
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_STATION_CAPACITY_COST_HPP
