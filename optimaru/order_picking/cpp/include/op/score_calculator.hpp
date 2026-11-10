#ifndef OP_SCORE_CALCULATOR_HPP
#define OP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "op/domain.hpp"

namespace op
{

// Per-constraint penalty totals of one full recompute (all counted positive;
// toScore() applies the signs), one per constraint.
// Each member is named after its constraint's registry name
// (op/constraints/<subject>_cost.hpp) and holds that constraint's
// contribution at its default weight: kDefaultWeight * raw cost, summed over
// the trolleys (score_calculator.cpp reads the components' constants). At the
// defaults that is the raw cost itself for the first three and 1000 * the raw
// (order, trolley) pair count for orderSplitByTrolleyCost. Used by the tests
// to assert each constraint against hand-computed cases, and by
// the demo output; Python reads the raw costs from constraint_costs() instead.
struct ConstraintTotals
{
    // Hard: sum over trolleys of max(0, requiredBuckets - bucketCount) where
    // requiredBuckets = sum over orders on the trolley of
    // ceil(orderVolumeOnTrolley / bucketCapacity) — buckets are not shared
    // between orders.
    std::int64_t trolleyCapacityCost = 0;

    // Soft: distance from the previous pick (or the trolley origin for the
    // first pick) to each pick.
    std::int64_t distanceFromPreviousPickTaskCost = 0;

    // Soft: distance from each trolley's last pick back to the path origin.
    std::int64_t distanceFromLastPickTaskToPathOriginCost = 0;

    // Soft: 1000 * distinct trolleys per order
    // (1000 = OrderSplitByTrolleyCost::kDefaultWeight).
    std::int64_t orderSplitByTrolleyCost = 0;

    Score toScore() const
    {
        return {-trolleyCapacityCost,
                static_cast<double>(-(distanceFromPreviousPickTaskCost
                                      + distanceFromLastPickTaskToPathOriginCost
                                      + orderSplitByTrolleyCost))};
    }
};

// The constraints as a full recompute — the reference
// the incremental calculator is checked against (in the tests and under
// ScoreChecks::ScoreChecks::Full).
//
// Constraints (Score), the default assembly of op/constraints.hpp at
// the components' kDefaultWeight (see ConstraintTotals):
//   hard 1. trolleyCapacityCost                        weight 1
//   soft 2. distanceFromPreviousPickTaskCost           weight 1
//   soft 3. distanceFromLastPickTaskToPathOriginCost   weight 1
//   soft 4. orderSplitByTrolleyCost                    weight 1000
//
// Unassigned pick tasks (present in no trolley's list) contribute nothing:
// the construction heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    // Recomputes every pick task's derived fields (trolleyIndex,
    // previousPickTask, nextPickTask) from the current trolley pick lists.
    // Tasks in no list get -1 for all three.
    static void updateShadowVariables(OrderPickingSolution &solution);

    // Refreshes the derived fields, then computes the per-constraint
    // totals (at the default weights) by reading them.
    static ConstraintTotals calculateTotals(OrderPickingSolution &solution);

    // calculateTotals folded into the Score, stored in solution.score
    // and returned.
    static Score score(OrderPickingSolution &solution);

    // Buckets needed for one order's volume on one trolley:
    // ceil(orderVolume / bucketVolume) — buckets are not shared across
    // orders.
    static std::int64_t calculateOrderRequiredBuckets(std::int64_t orderVolume,
                                                      std::int64_t bucketVolume)
    {
        return (orderVolume + (bucketVolume - 1)) / bucketVolume;
    }
};

}  // namespace op

#endif  // OP_SCORE_CALCULATOR_HPP
