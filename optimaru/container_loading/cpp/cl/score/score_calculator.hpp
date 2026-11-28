#ifndef CL_SCORE_CALCULATOR_HPP
#define CL_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "cl/model/domain.hpp"

namespace cl {

// The raw cost of every constraint on one solution, from one full recompute:
// each member is named after its constraint's registry name
// (cl/constraints/<subject>_cost.hpp) and holds that constraint's raw cost
// summed over the containers (groupSplitCost: over the groups). toScore()
// applies the components' default weights and levels, so the two opt-in costs
// (weight 0) are reported here and leave the score alone.
struct ConstraintTotals {
    std::int64_t unplacedItemCost = 0;     // hard: items of a list that found no place
    std::int64_t containerWeightCost = 0;  // hard: weight of a list above the container's limit
    std::int64_t stackingLoadCost = 0;     // hard: weight resting on an item above its limit
    std::int64_t unloadingOrderCost = 0;   // hard: pairs where a later item blocks an earlier one
    double usedContainerCost = 0.0;        // soft: cost of the containers holding a placed item
    std::int64_t groupSplitCost = 0;       // soft: containers a group is in, beyond its first
    std::int64_t usedLengthCost = 0;       // soft, opt-in: used length, summed
    std::int64_t loadBalanceCost = 0;      // soft, opt-in: centre-of-gravity offsets, summed

    Score toScore() const;
};

// The constraints as a full recompute, without the engine: every container's
// list is decoded from scratch and every cost summed. It is what the default
// assembly of cl/constraints.hpp is checked against in the tests, and what
// scores the solution a solve returns.
//
// An item in no container's list contributes nothing: the engine scores
// partially built solutions while it constructs one or rebuilds a ruin.
class ScoreCalculator {
public:
    // Checks the problem and builds what the decoder and the costs read off
    // it (the orientations in the order the decoder tries them, the groups
    // numbered densely). Every solve and every scoring call runs it; it is
    // idempotent. Throws std::invalid_argument naming what is wrong.
    static void prepare(ContainerLoadingProblem& problem);

    // Decodes every container and writes the derived fields: each item's
    // container, sequence, position and placed size, each container's placed
    // and unplaced items, loaded weight and volume and used length.
    static void updateDerived(ContainerLoadingProblem& problem);

    static ConstraintTotals calculateTotals(ContainerLoadingProblem& problem);

    // prepare + updateDerived + the score at the default weights; also stores
    // it in problem.score.
    static Score score(ContainerLoadingProblem& problem);
};

}  // namespace cl

#endif  // CL_SCORE_CALCULATOR_HPP
