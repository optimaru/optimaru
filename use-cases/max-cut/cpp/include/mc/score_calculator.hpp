#ifndef MC_SCORE_CALCULATOR_HPP
#define MC_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "mc/domain.hpp"

namespace mc
{

// The raw cost of every default constraint on the current partition, before
// weight / exponent / level (the unit-constraint costs per side, the globals
// as one total). Read straight from the facts: the reference the incremental
// calculator, the plugin components and the tests are checked against.
struct RawCosts
{
    std::int64_t fixed[kSideCount] = {0, 0};      // fixedPartitionCost
    std::int64_t same[kSideCount] = {0, 0};       // samePartitionPairsCost
    std::int64_t different[kSideCount] = {0, 0};  // differentPartitionPairsCost
    std::int64_t size[kSideCount] = {0, 0};       // partitionSizeBoundsCost
    std::int64_t imbalance = 0;                   // partitionImbalanceCost
    std::int64_t cut = 0;                         // cutWeightCost = -cut

    std::int64_t fixedTotal() const { return fixed[0] + fixed[1]; }
    std::int64_t sameTotal() const { return same[0] + same[1]; }
    std::int64_t differentTotal() const { return different[0] + different[1]; }
    std::int64_t sizeTotal() const { return size[0] + size[1]; }
};

// The max-cut score, monolithic.
//
// calculateScore does a full recompute straight from the facts (it reads none
// of the derived indexes) — the reference the incremental calculator and the
// plugin assembly are checked against, in the tests and under FULL_ASSERT.
//
// Constraints (HardSoftScore), the default assembly of mc/constraints.hpp at
// the components' kDefaultWeight:
//   hard 1. fixedPartitionCost           fixed vertices off their fixed side
//   hard 2. samePartitionPairsCost       same-side pairs split (charged to
//                                        the first endpoint's side)
//   hard 3. differentPartitionPairsCost  different-side pairs on one side
//   hard 4. partitionSizeBoundsCost      per side max(0, min - n) +
//                                        max(0, n - max)
//   soft 5. partitionImbalanceCost       |n0 - n1|, opt-in (weight 0)
//   soft 6. cutWeightCost                a reward: -(cut weight), so the soft
//                                        score is +cut weight
// Unassigned vertices (-1) follow the rule documented in mc/domain.hpp.
class ScoreCalculator
{
public:
    // Runs prepare(), then the full recompute; stores problem.score and sets
    // problem.scored.
    static Score calculateScore(MaxCutProblem &problem);

    // The raw costs (no prepare needed beyond valid indices; calculateScore
    // runs prepare first).
    static RawCosts rawCosts(const MaxCutProblem &problem);

    // The default-weight score of `raw`.
    static Score scoreOf(const RawCosts &raw);

    // Validates the facts and the partition and (re)builds the derived
    // indexes (MaxCutProblem's "derived data"). An empty `side` / `fixedSide`
    // is sized to the vertex count first (all -1). Throws
    // std::invalid_argument naming the offending fact: a duplicate vertex
    // id, an edge or pair index out of range, a negative edge weight, a side
    // or fixed side outside {-1, 0, 1}, a side / fixedSide vector of the
    // wrong length, or size bounds outside 0 <= min <= max <= |V|.
    // Always rebuilds (cheap, O(vertices + edges + pairs)).
    static void prepare(MaxCutProblem &problem);
};

}  // namespace mc

#endif  // MC_SCORE_CALCULATOR_HPP
