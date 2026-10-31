#include "op/incremental_calculator.hpp"

#include <cstdint>

#include "op/constraints/distance_from_last_pick_task_to_path_origin_cost.hpp"
#include "op/constraints/distance_from_previous_pick_task_cost.hpp"
#include "op/constraints/order_split_by_trolley_cost.hpp"
#include "op/constraints/trolley_capacity_cost.hpp"
#include "op/score_calculator.hpp"

namespace op
{

namespace
{

// The same constants the components and ScoreCalculator read (integral and
// level-checked; see constraints/common.hpp).
constexpr std::int64_t kCapacityWeight = kHardWeight<TrolleyCapacityCost>;
constexpr std::int64_t kDistanceWeight
    = kSoftWeight<DistanceFromPreviousPickTaskCost>;
constexpr std::int64_t kSplitWeight = kSoftWeight<OrderSplitByTrolleyCost>;
// dist_ keeps each trolley's whole round trip (the outbound legs of
// distanceFromPreviousPickTaskCost plus the return leg of
// distanceFromLastPickTaskToPathOriginCost) as ONE number, so both distance
// constraints must default to the same weight.
static_assert(kDistanceWeight
                  == kSoftWeight<DistanceFromLastPickTaskToPathOriginCost>,
              "the round-trip distance total assumes equal default weights "
              "for the two distance constraints");

}  // namespace

void OrderPickingIncrementalScoreCalculator::resetWorkingSolution(
    OrderPickingSolution &workingSolution)
{
    solution_ = &workingSolution;
    trolleyCount_ = static_cast<int>(workingSolution.trolleys.size());
    orderCount_ = static_cast<int>(workingSolution.orders.size());

    volume_.assign(static_cast<std::size_t>(orderCount_) * trolleyCount_, 0);
    count_.assign(static_cast<std::size_t>(orderCount_) * trolleyCount_, 0);
    required_.assign(trolleyCount_, 0);
    shortage_.assign(trolleyCount_, 0);
    dist_.assign(trolleyCount_, 0);
    distinctTrolleys_.assign(orderCount_, 0);
    orderStamp_.assign(orderCount_, 0);
    stampGeneration_ = 0;
    shortageTotal_ = 0;
    distTotal_ = 0;
    splitTotal_ = 0;

    for (int t = 0; t < trolleyCount_; ++t)
    {
        for (int pickIndex : workingSolution.trolleys[t].pickTaskIndices)
        {
            const PickTask &task = workingSolution.pickTasks[pickIndex];
            volume_[cell(task.orderIndex, t)] += task.volume;
            if (count_[cell(task.orderIndex, t)]++ == 0)
            {
                ++distinctTrolleys_[task.orderIndex];
                ++splitTotal_;
            }
        }
        const Trolley &trolley = workingSolution.trolleys[t];
        const std::int64_t required = requiredBuckets(t);
        required_[t] = required;
        shortage_[t] = required > trolley.bucketCount
                           ? required - trolley.bucketCount
                           : 0;
        shortageTotal_ += shortage_[t];
        dist_[t] = roundTripDistance(t);
        distTotal_ += dist_[t];
    }
}

void OrderPickingIncrementalScoreCalculator::beforeListChanged(int listIndex)
{
    // Retract the trolley's whole contribution against its OLD contents.
    shortageTotal_ -= shortage_[listIndex];
    distTotal_ -= dist_[listIndex];
    for (int pickIndex : solution_->trolleys[listIndex].pickTaskIndices)
    {
        const PickTask &task = solution_->pickTasks[pickIndex];
        volume_[cell(task.orderIndex, listIndex)] -= task.volume;
        if (--count_[cell(task.orderIndex, listIndex)] == 0)
        {
            --distinctTrolleys_[task.orderIndex];
            --splitTotal_;
        }
    }
}

void OrderPickingIncrementalScoreCalculator::afterListChanged(int listIndex)
{
    // Insert the trolley's contribution from its NEW contents.
    for (int pickIndex : solution_->trolleys[listIndex].pickTaskIndices)
    {
        const PickTask &task = solution_->pickTasks[pickIndex];
        volume_[cell(task.orderIndex, listIndex)] += task.volume;
        if (count_[cell(task.orderIndex, listIndex)]++ == 0)
        {
            ++distinctTrolleys_[task.orderIndex];
            ++splitTotal_;
        }
    }
    const Trolley &trolley = solution_->trolleys[listIndex];
    const std::int64_t required = requiredBuckets(listIndex);
    required_[listIndex] = required;
    shortage_[listIndex]
        = required > trolley.bucketCount ? required - trolley.bucketCount : 0;
    shortageTotal_ += shortage_[listIndex];
    dist_[listIndex] = roundTripDistance(listIndex);
    distTotal_ += dist_[listIndex];
}

Score OrderPickingIncrementalScoreCalculator::calculateScore()
{
    return {-kCapacityWeight * shortageTotal_,
            static_cast<double>(
                -(kDistanceWeight * distTotal_ + kSplitWeight * splitTotal_))};
}

std::int64_t
OrderPickingIncrementalScoreCalculator::requiredBuckets(int trolley)
{
    ++stampGeneration_;
    std::int64_t buckets = 0;
    const Trolley &owner = solution_->trolleys[trolley];
    for (int pickIndex : owner.pickTaskIndices)
    {
        const int order = solution_->pickTasks[pickIndex].orderIndex;
        if (orderStamp_[order] == stampGeneration_)
            continue;  // this order's volume was already counted
        orderStamp_[order] = stampGeneration_;
        buckets += ScoreCalculator::calculateOrderRequiredBuckets(
            volume_[cell(order, trolley)], owner.bucketCapacity);
    }
    return buckets;
}

std::int64_t
OrderPickingIncrementalScoreCalculator::roundTripDistance(int trolley) const
{
    return calculateDistanceToTravel(*solution_, solution_->trolleys[trolley]);
}

}  // namespace op
