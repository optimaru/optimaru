#ifndef OP_INCREMENTAL_CALCULATOR_HPP
#define OP_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "op/domain.hpp"
#include "optimaru/score/objective.hpp"

namespace op
{

// The order-picking implementation of the engine's
// optimaru::Objective.
//
// LIST-kind hooks, per-trolley granularity (the vehicle-routing pattern): a
// move touches at most two trolleys, and every constraint decomposes into
// per-trolley terms plus the per-(order, trolley) presence matrix:
//   - soft distance: dist_[t] = the trolley's round-trip travel distance
//     (origin -> picks in order -> origin), the sum of the two distance
//     constraints (distanceFromPreviousPickTaskCost +
//     distanceFromLastPickTaskToPathOriginCost). Re-derived per touched list,
//     O(list).
//   - hard buckets (trolleyCapacityCost): volume_[o][t] and count_[o][t]
//     maintained per element; the trolley's required buckets = sum over its
//     orders of ceil(volume / bucketCapacity), re-derived per touched list.
//   - soft order split (orderSplitByTrolleyCost): distinct trolleys per
//     order == number of t with count_[o][t] > 0, maintained on the 0 <-> >0
//     transitions.
// score applies the components' kDefaultWeight (1 / 1 / 1 / 1000).
//
// before_list_edit retracts the trolley's contribution while its list still
// has the OLD contents; after_list_edit inserts the NEW contents. Pick tasks
// in no list contribute nothing (the construction heuristic scores partially
// initialized solutions). This calculator reads the pick lists directly and
// never the derived fields, so it stays correct while the engine mutates
// lists mid-move (they are refreshed by ScoreCalculator afterwards). The
// full-recompute reference is ScoreCalculator::score (checked in
// the tests and under ScoreChecks::ScoreChecks::Full).
class OrderPickingIncrementalScoreCalculator final
    : public optimaru::Objective<OrderPickingSolution>
{
public:
    void rebuild(OrderPickingSolution &solution) override;
    void before_list_edit(int listIndex) override;
    void after_list_edit(int listIndex) override;
    Score score() override;

    // Read-only views of the live (order, trolley) presence table and the
    // per-trolley bucket demand, used by the order-consolidation move
    // operator (see Solver) to find split orders, their holder trolleys and
    // merge feasibility in O(1) per cell.
    int distinctTrolleyCount(int orderIndex) const
    {
        return distinctTrolleys_[orderIndex];
    }
    int pickCountOnTrolley(int orderIndex, int trolley) const
    {
        return count_[cell(orderIndex, trolley)];
    }
    std::int64_t orderVolumeOnTrolley(int orderIndex, int trolley) const
    {
        return volume_[cell(orderIndex, trolley)];
    }
    std::int64_t requiredBucketsOnTrolley(int trolley) const
    {
        return required_[trolley];
    }

private:
    OrderPickingSolution *solution_ = nullptr;
    int trolleyCount_ = 0;
    int orderCount_ = 0;

    // Per-(order, trolley) totals over the assigned picks.
    std::vector<std::int64_t> volume_;  // orderIndex * trolleyCount_ + trolley
    std::vector<int> count_;

    // Per-trolley derived state.
    std::vector<std::int64_t> required_;  // total required buckets
    std::vector<std::int64_t> shortage_;  // max(0, requiredBuckets - buckets)
    std::vector<std::int64_t> dist_;      // round-trip travel distance

    // Per-order distinct-trolley count (order split).
    std::vector<int> distinctTrolleys_;

    // Running totals.
    std::int64_t shortageTotal_ = 0;  // hard
    std::int64_t distTotal_ = 0;      // soft, meters
    std::int64_t splitTotal_ = 0;     // soft, distinct (order, trolley) pairs

    std::size_t cell(int orderIndex, int trolley) const
    {
        return static_cast<std::size_t>(orderIndex) * trolleyCount_ + trolley;
    }

    // Scratch used to visit each order at most once while walking a list.
    std::vector<int> orderStamp_;
    int stampGeneration_ = 0;

    // Required buckets and round-trip distance of one trolley, from its
    // current list and the volume_ table.
    std::int64_t requiredBuckets(int trolley);
    std::int64_t roundTripDistance(int trolley) const;
};

}  // namespace op

#endif  // OP_INCREMENTAL_CALCULATOR_HPP
