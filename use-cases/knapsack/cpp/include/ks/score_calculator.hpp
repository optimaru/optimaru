#ifndef KS_SCORE_CALCULATOR_HPP
#define KS_SCORE_CALCULATOR_HPP

#include "ks/domain.hpp"

namespace ks
{

// The knapsack score, monolithic.
//
// calculateScore does a full recompute straight from the facts (it reads none
// of the derived indexes) — the reference the incremental calculator and the
// plugin assembly are checked against, in the tests and under FULL_ASSERT.
//
// Constraints (HardSoftScore), the default assembly of ks/constraints.hpp at
// the components' kDefaultWeight (every one 1):
//   hard 1. knapsackCapacityCost        max(0, selected weight - capacity)
//   hard 2. resourceCapacityCost        sum over declared resources of
//                                       max(0, selected usage - capacity)
//   hard 3. mandatoryItemsCost          mandatory entries not selected
//   hard 4. forbiddenItemsCost          forbidden entries selected
//   hard 5. selectionGroupBoundsCost    sum over groups of
//                                       max(0, min - n) + max(0, n - max)
//   hard 6. itemDependencyCost          (item, required) pairs with the item
//                                       selected and the required one not
//   soft 7. selectedValueCost           a reward: -(selected value), so the
//                                       soft score is +selected value
// Undecided items (selected == -1) count as not selected.
class ScoreCalculator
{
public:
    // Runs prepare(), then the full recompute; stores problem.score.
    static Score calculateScore(KnapsackProblem &problem);

    // Validates the facts and the selection and (re)builds the derived
    // indexes (KnapsackProblem's "derived data"). Throws std::invalid_argument
    // naming the offending fact: an index out of range, weight <= 0, a
    // negative value / capacity / usage, a duplicate item or resource id, a
    // group listing an item twice or with bounds outside 0 <= min <= max <=
    // size, an item both mandatory and forbidden, or a selection outside
    // {-1, 0, 1}. Always rebuilds (cheap, O(items + references)).
    static void prepare(KnapsackProblem &problem);
};

}  // namespace ks

#endif  // KS_SCORE_CALCULATOR_HPP
