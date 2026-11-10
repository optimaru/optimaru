#ifndef MC_CONSTRAINTS_DIFFERENT_PARTITION_PAIRS_COST_HPP
#define MC_CONSTRAINTS_DIFFERENT_PARTITION_PAIRS_COST_HPP

#include <vector>

#include "mc/constraints/common.hpp"

namespace mc
{

// Hard, weight 1: differentPartitionPairsCost — must-be-apart pairs that sit
// on one side. Raw cost per side u = the pairs with both endpoints on side u,
// so 0 = every pair is split. The two sides sum to the count of joined
// pairs. A self-pair is always joined
// (1 on its vertex's side); a repeated pair counts once per entry.
//
// cost() is a pure function of (unit, members) (a pair counts only when both
// endpoints are inside the unit). delta_cost(e) =
//   #{pairs naming e and w != e, w on side u}  +  #{(e, e) self-pairs}
// read from the CSR incidence (O(pair degree)); partner sides come from the
// working solution (see common.hpp for why that is exact).
struct DifferentPartitionPairsCost final : McUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DifferentPartitionPairsCost(double weight = kDefaultWeight)
        : McUnitConstraint("differentPartitionPairsCost", weight, kDefaultLevel)
    {
    }

    void on_reset(const MaxCutProblem &problem) override
    {
        detail::requirePrepared(problem);
    }

    double cost(const MaxCutProblem &problem, int /*unit*/, const std::vector<int> &members) const override
    {
        const std::vector<char> in = detail::memberFlags(problem, members);
        double total = 0.0;
        for (const VertexPair &pair : problem.differentPartitionPairs)
        {
            if (in[pair.first] && in[pair.second])
                total += 1.0;
        }
        return total;
    }

    bool supports_delta() const override { return true; }
    double delta_cost(const MaxCutProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int vertex) const override
    {
        double delta = problem.diffSelfCount[vertex];
        for (int k = problem.diffStart[vertex]; k < problem.diffStart[vertex + 1]; ++k)
        {
            if (problem.side[problem.diffPartner[k]] == unit)
                delta += 1.0;
        }
        return delta;
    }
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_DIFFERENT_PARTITION_PAIRS_COST_HPP
