#ifndef OP_SCORE_CALCULATOR_HPP
#define OP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "op/domain.hpp"

namespace op
{

// Per-constraint penalty totals (all counted positive), matching the four
// constraints of org.acme.orderpicking.solver.OrderPickingConstraintProvider
// one to one. Used by the tests to assert each constraint against the Java
// ConstraintVerifier cases, and by the demo output.
struct ConstraintTotals
{
    // Hard: sum over trolleys of max(0, requiredBuckets - bucketCount) where
    // requiredBuckets = sum over orders on the trolley of
    // ceil(orderVolumeOnTrolley / bucketCapacity) — buckets are not shared
    // between orders ("Required number of buckets").
    std::int64_t requiredNumberOfBuckets = 0;

    // Soft: distance from the previous pick (or the trolley origin for the
    // first pick) to each pick ("Minimize the distance from the previous
    // trolley pick").
    std::int64_t distanceFromPreviousPickTask = 0;

    // Soft: distance from each trolley's last pick back to the path origin
    // ("Minimize the distance from last trolley pick to the path origin").
    std::int64_t distanceFromLastPickTaskToPathOrigin = 0;

    // Soft: 1000 * countDistinct(trolley) per order ("Minimize order split by
    // trolley").
    std::int64_t orderSplitByTrolley = 0;

    Score toScore() const
    {
        return {-requiredNumberOfBuckets,
                static_cast<double>(-(distanceFromPreviousPickTask
                                      + distanceFromLastPickTaskToPathOrigin
                                      + orderSplitByTrolley))};
    }
};

// Port of OrderPickingConstraintProvider as a full recompute — the reference
// the incremental calculator is checked against (in the tests and under
// EnvironmentMode::FULL_ASSERT).
//
// Constraints (HardSoftScore, matching the Java constraint names):
//   hard 1. requiredNumberOfBuckets           see ConstraintTotals
//   soft 2. minimizeDistanceFromPreviousPickTask
//   soft 3. minimizeDistanceFromLastPickTaskToPathOrigin
//   soft 4. minimizeOrderSplitByTrolley
//
// Unassigned pick tasks (present in no trolley's list) contribute nothing,
// mirroring Timefold's forEach(PickTask.class) which only iterates tasks
// assigned to a trolley: the construction heuristic scores partially
// initialized solutions.
class ScoreCalculator
{
public:
    // Recomputes every pick task's shadow variables (trolleyIndex,
    // previousPickTask, nextPickTask) from the current trolley pick lists —
    // the C++ analog of the Java shadow-variable updates
    // (@InverseRelationShadowVariable / @PreviousElementShadowVariable /
    // @NextElementShadowVariable). Tasks in no list get -1 (null) for all
    // three.
    static void updateShadowVariables(OrderPickingSolution &solution);

    // Refreshes the shadow variables, then computes the per-constraint
    // totals by reading them — a literal transcription of the Java
    // constraint definitions.
    static ConstraintTotals calculateTotals(OrderPickingSolution &solution);

    // calculateTotals folded into the HardSoftScore, stored in solution.score
    // and returned.
    static Score calculateScore(OrderPickingSolution &solution);

    // Buckets needed for one order's volume on one trolley:
    // ceil(orderVolume / bucketVolume) — buckets are not shared across
    // orders (OrderPickingConstraintProvider.calculateOrderRequiredBuckets).
    static std::int64_t calculateOrderRequiredBuckets(std::int64_t orderVolume,
                                                      std::int64_t bucketVolume)
    {
        return (orderVolume + (bucketVolume - 1)) / bucketVolume;
    }
};

}  // namespace op

#endif  // OP_SCORE_CALCULATOR_HPP
