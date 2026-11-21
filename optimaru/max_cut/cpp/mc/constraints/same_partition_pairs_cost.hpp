#ifndef MC_CONSTRAINTS_SAME_PARTITION_PAIRS_COST_HPP
#define MC_CONSTRAINTS_SAME_PARTITION_PAIRS_COST_HPP

#include <vector>

#include "mc/constraints/common.hpp"

namespace mc {

// Hard, weight 1: samePartitionPairsCost — must-be-together pairs that are
// split. Raw cost per side u = the pairs whose FIRST endpoint is on side u
// and whose second endpoint is not, so each split pair is charged once, to
// its first endpoint's side, and 0 = every pair is together. The two sides
// sum to the count of split pairs.
// A self-pair is never split; a repeated pair counts once per entry.
//
// cost() is a pure function of (unit, members). delta_cost(e) =
//   #{pairs (e, w), w != e, w not on side u}  -  #{pairs (w, e), w != e, w on side u}
// read from the CSR incidence (O(pair degree)); partner sides come from the
// working solution (see common.hpp for why that is exact).
struct SamePartitionPairsCost final : McUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SamePartitionPairsCost(double weight = kDefaultWeight)
        : McUnitConstraint("samePartitionPairsCost", weight, kDefaultLevel) {}

    void on_reset(const MaxCutProblem& problem) override { detail::requirePrepared(problem); }

    double cost(const MaxCutProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        const std::vector<char> in = detail::memberFlags(problem, members);
        double total = 0.0;
        for (const VertexPair& pair : problem.samePartitionPairs) {
            if (in[pair.first] && !in[pair.second]) total += 1.0;
        }
        return total;
    }

    bool supports_delta() const override { return true; }
    double delta_cost(const MaxCutProblem& problem, int unit, const std::vector<int>& /*members*/,
                      int vertex) const override {
        double delta = 0.0;
        for (int k = problem.sameAsFirstStart[vertex]; k < problem.sameAsFirstStart[vertex + 1];
             ++k) {
            if (problem.side[problem.sameAsFirstPartner[k]] != unit) delta += 1.0;
        }
        for (int k = problem.sameAsSecondStart[vertex]; k < problem.sameAsSecondStart[vertex + 1];
             ++k) {
            if (problem.side[problem.sameAsSecondPartner[k]] == unit) delta -= 1.0;
        }
        return delta;
    }
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_SAME_PARTITION_PAIRS_COST_HPP
