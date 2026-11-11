#ifndef PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP
#define PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP

#include <vector>

#include "optimaru/score/group_placement.hpp"
#include "pdp/constraints/common.hpp"

namespace pdp {

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty): requestGroupCost — the
// requests of one group (Delivery::groupIndex) ride one vehicle together, or
// all wait. Orders the input has merged into one job stay one job.
//
// A request is where its pickup is: on a vehicle, or on none (the unassigned
// pool). Raw cost (global) = over the groups, the requests that are not where
// most of their group is, so 0 = every group is whole (or nothing is grouped).
// A group split 2 + 1 costs 1, one spread over three places 2: every request
// that joins the others takes 1 off, which is the slope a stop-by-stop repair
// follows back to a whole group. The counting is the engine's
// optimaru::GroupPlacement, over the pickups.
//
// Silent about a request whose own pickup and dropoff are apart:
// pairedRequestCost and sameVehicleCost price that.
struct RequestGroupCost final : PdpGlobalConstraint {
    static constexpr double kDefaultWeight = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequestGroupCost(double weight = kDefaultWeight)
        : PdpGlobalConstraint("requestGroupCost", weight, kDefaultLevel) {}

    void reset(const PdpRoutePlan& plan,
               const std::vector<std::vector<int>>& members_per_unit) override {
        std::vector<int> groupOf(plan.nodes.size(), -1);
        for (const Delivery& delivery : plan.deliveries) {
            if (delivery.groupIndex >= 0)
                groupOf[static_cast<std::size_t>(delivery.pickupNodeIndex)] = delivery.groupIndex;
        }
        // The vehicles are the places; the pool after them is "on no vehicle".
        placement_.reset(groupOf, members_per_unit, static_cast<int>(plan.vehicles.size()));
    }

    void on_unit_changed(const PdpRoutePlan& /*plan*/, int unit,
                         const std::vector<int>& members) override {
        placement_.on_list_changed(unit, members);
    }

    double cost() const override { return static_cast<double>(placement_.apart()); }

    bool supports_preview() const override { return true; }
    double preview_cost(const PdpRoutePlan& /*plan*/, const optimaru::PreviewedUnit* changes,
                        int count) const override {
        return static_cast<double>(placement_.preview(changes, count));
    }

private:
    optimaru::GroupPlacement placement_;
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP
