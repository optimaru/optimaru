#ifndef VRP_CONSTRAINTS_VISIT_GROUP_COST_HPP
#define VRP_CONSTRAINTS_VISIT_GROUP_COST_HPP

#include <vector>

#include "optimaru/score/group_placement.hpp"
#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1000: visitGroupCost -- the visits of one group
// (Visit::groupIndex) ride one vehicle together, or all wait. Orders the input
// has merged into one job stay one job.
//
// Raw cost (global) = over the groups, the visits that are not where most of
// their group is (on a vehicle, or on none), so 0 = every group is whole (or
// nothing is grouped). A group split 2 + 1 costs 1, one spread over three
// places 2: every visit that joins the others takes 1 off, which is the slope
// the engine's one-visit moves follow back to a whole group. The weight is
// far above a typical lateness or overload, so a split group is mended before
// windows are polished.
//
// Global because a group spans routes. The counting is the engine's
// optimaru::GroupPlacement, which also answers the move previews from the
// candidate routes, so an assembly with this constraint still serves them.
struct VisitGroupCost final : VrpGlobalConstraint {
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VisitGroupCost(double weight = kDefaultWeight)
        : VrpGlobalConstraint("visitGroupCost", weight, kDefaultLevel) {}

    void reset(const VehicleRoutePlan& plan,
               const std::vector<std::vector<int>>& members_per_unit) override {
        std::vector<int> groupOf(plan.visits.size(), -1);
        for (std::size_t v = 0; v < plan.visits.size(); ++v) groupOf[v] = plan.visits[v].groupIndex;
        // The vehicles are the places; the pool after them is "on no vehicle".
        placement_.reset(groupOf, members_per_unit, static_cast<int>(plan.vehicles.size()));
    }

    void on_unit_changed(const VehicleRoutePlan& /*plan*/, int unit,
                         const std::vector<int>& members) override {
        placement_.on_list_changed(unit, members);
    }

    double cost() const override { return static_cast<double>(placement_.apart()); }

    bool supports_preview() const override { return true; }
    double preview_cost(const VehicleRoutePlan& /*plan*/, const optimaru::PreviewedUnit* changes,
                        int count) const override {
        return static_cast<double>(placement_.preview(changes, count));
    }

private:
    optimaru::GroupPlacement placement_;
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_VISIT_GROUP_COST_HPP
