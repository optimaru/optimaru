#ifndef KS_SCORE_CALCULATOR_HPP
#define KS_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "ks/domain.hpp"

namespace ks {

// The knapsack score, monolithic.
//
// score does a full recompute straight from the facts (it reads none
// of the derived indexes) — the reference the incremental calculator and the
// plugin assembly are checked against, in the tests and under ScoreChecks::Full.
//
// Constraints (Score), the default assembly of ks/constraints.hpp at
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

// The raw cost of every default constraint, all >= 0 (signs and weights are
// applied in the score): the selected weight over capacity, the summed
// resource overflow, the unselected mandatory and selected forbidden
// entries, the summed group violation, the unmet dependencies, and the
// selected value (+value; selectedValueCost is -value).
struct RawCosts {
    std::int64_t capacityOverflow = 0;   // knapsackCapacityCost
    std::int64_t resourceOverflow = 0;   // resourceCapacityCost
    std::int64_t missingMandatory = 0;   // mandatoryItemsCost
    std::int64_t chosenForbidden = 0;    // forbiddenItemsCost
    std::int64_t groupViolation = 0;     // selectionGroupBoundsCost
    std::int64_t unmetDependencies = 0;  // itemDependencyCost
    std::int64_t value = 0;              // selectedValueCost (as +value)

    bool operator==(const RawCosts& other) const {
        return capacityOverflow == other.capacityOverflow &&
               resourceOverflow == other.resourceOverflow &&
               missingMandatory == other.missingMandatory &&
               chosenForbidden == other.chosenForbidden && groupViolation == other.groupViolation &&
               unmetDependencies == other.unmetDependencies && value == other.value;
    }
    bool operator!=(const RawCosts& other) const { return !(*this == other); }
};

class ScoreCalculator {
public:
    // Runs prepare(), then the full recompute; stores problem.score.
    static Score score(KnapsackProblem& problem);

    // Runs prepare(), then the full recompute of every raw cost (what
    // score weighs); leaves problem.score alone.
    static RawCosts rawCosts(KnapsackProblem& problem);

    // Validates the facts and the selection and (re)builds the derived
    // indexes (KnapsackProblem's "derived data"). Throws std::invalid_argument
    // naming the offending fact: an index out of range, weight <= 0, a
    // negative value / capacity / usage, a duplicate item or resource id, a
    // group listing an item twice or with bounds outside 0 <= min <= max <=
    // size, an item both mandatory and forbidden, or a selection outside
    // {-1, 0, 1}. Always rebuilds (cheap, O(items + references)).
    static void prepare(KnapsackProblem& problem);
};

}  // namespace ks

#endif  // KS_SCORE_CALCULATOR_HPP
