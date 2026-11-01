#ifndef MC_CONSTRAINTS_FIXED_PARTITION_COST_HPP
#define MC_CONSTRAINTS_FIXED_PARTITION_COST_HPP

#include <vector>

#include "mc/constraints/common.hpp"

namespace mc
{

// Hard, weight 1: fixedPartitionCost — fixed vertices (fixedSide >= 0) that
// sit on the other side. Raw cost per side u = the members v of side u with
// fixedSide[v] >= 0 and fixedSide[v] != u, so 0 = every fixed vertex is on
// its side. The two sides sum to the 0.1.x count of fixed vertices off their
// side (0.1.x listed one 0/1 term per fixed vertex). The engine never moves a
// pinned (fixed) vertex, so during search it stays 0; only a warm-start seed
// that contradicts fixed_partition makes it positive. Per-member delta: O(1).
struct FixedPartitionCost final : McUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FixedPartitionCost(double weight = kDefaultWeight)
        : McUnitConstraint("fixedPartitionCost", weight, kDefaultLevel)
    {
    }

    void onReset(const MaxCutProblem &problem) override
    {
        detail::requirePrepared(problem);
    }

    double cost(const MaxCutProblem &problem, int unit, const std::vector<int> &members) const override
    {
        double total = 0.0;
        for (int v : members)
            total += offSide(problem, unit, v);
        return total;
    }

    bool supportsDelta() const override { return true; }
    double deltaCost(const MaxCutProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int vertex) const override
    {
        return offSide(problem, unit, vertex);
    }

private:
    static double offSide(const MaxCutProblem &problem, int unit, int vertex)
    {
        const int fixed = problem.fixedSide[vertex];
        return fixed >= 0 && fixed != unit ? 1.0 : 0.0;
    }
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_FIXED_PARTITION_COST_HPP
