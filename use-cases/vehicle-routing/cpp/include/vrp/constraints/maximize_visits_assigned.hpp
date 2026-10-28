#ifndef VRP_CONSTRAINTS_MAXIMIZE_VISITS_ASSIGNED_HPP
#define VRP_CONSTRAINTS_MAXIMIZE_VISITS_ASSIGNED_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Medium (default, weight 1 medium level): maximizeVisitsAssigned -- the
// per-visit penalty of every visit left in no route (by default its service
// duration in minutes; see Visit::unassignedPenalty).
//
// Global because an unrouted visit belongs to no unit. It keeps the routed
// penalty per vehicle, so a touched route costs O(its visits) and the total is
// one subtraction; the same per-vehicle table plus the shared penalty sums
// answer the engine's move previews in O(pieces), which is what lets the
// whole default assembly serve them without writing a candidate route out.
//
// The medium level is folded into the engine's soft level as
// `medium * kMediumUnit` (see domain.hpp), which is what the default weight
// carries; the component's own cost stays plain penalty units.
struct MaximizeVisitsAssigned final : detail::SummedGlobalConstraint {
    explicit MaximizeVisitsAssigned(double weight = static_cast<double>(kMediumUnit))
        : SummedGlobalConstraint("maximizeVisitsAssigned", weight, /*hard=*/false) {}

    void resetWithSums(const VehicleRoutePlan& plan,
                       const std::vector<std::vector<int>>& membersPerUnit) override {
        totalPenalty_ = 0;
        for (const Visit& visit : plan.visits) totalPenalty_ += visit.unassignedPenalty();
        routedPerVehicle_.assign(membersPerUnit.size(), 0);
        routedPenalty_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit) {
            if (detail::isPool(plan, static_cast<int>(unit))) continue;
            routedPerVehicle_[unit] = penaltyOf(plan, membersPerUnit[unit]);
            routedPenalty_ += routedPerVehicle_[unit];
        }
    }

    void onUnitChanged(const VehicleRoutePlan& plan, int unit,
                       const std::vector<int>& members) override {
        // A visit sitting in the pool is unrouted, exactly like one in no list
        // at all, so the pool's own contents never count as routed.
        if (detail::isPool(plan, unit)) return;
        const std::int64_t routed = penaltyOf(plan, members);
        routedPenalty_ += routed - routedPerVehicle_[unit];
        routedPerVehicle_[unit] = routed;
    }

    double cost() const override {
        return static_cast<double>(totalPenalty_ - routedPenalty_);
    }

    // Preview contract: the routed penalty with each previewed vehicle's
    // candidate members in place of its current ones. O(touched visits).
    bool supportsPreview() const override { return true; }
    double previewCost(const VehicleRoutePlan& plan, const optimaru::PreviewedUnit* changes,
                       int count) const override {
        std::int64_t routed = routedPenalty_;
        for (int i = 0; i < count; ++i) {
            if (detail::isPool(plan, changes[i].unit)) continue;
            routed += penaltyOf(plan, *changes[i].members)
                      - routedPerVehicle_[static_cast<std::size_t>(changes[i].unit)];
        }
        return static_cast<double>(totalPenalty_ - routed);
    }
    // The same from the shared penalty sums, O(pieces).
    bool piecewisePreviewCost(const VehicleRoutePlan& plan, const optimaru::PreviewedUnit* changes,
                              int count, double& out) const override {
        std::int64_t routed = routedPenalty_;
        for (int i = 0; i < count; ++i) {
            if (detail::isPool(plan, changes[i].unit)) continue;
            std::int64_t penalty = 0;
            for (int p = 0; p < changes[i].pieceCount; ++p) {
                penalty += sums_->penaltyOf(changes[i].pieces[p]);
            }
            routed += penalty - routedPerVehicle_[static_cast<std::size_t>(changes[i].unit)];
        }
        out = static_cast<double>(totalPenalty_ - routed);
        return true;
    }

private:
    static std::int64_t penaltyOf(const VehicleRoutePlan& plan,
                                  const std::vector<int>& members) {
        std::int64_t penalty = 0;
        for (int visitIndex : members) penalty += plan.visits[visitIndex].unassignedPenalty();
        return penalty;
    }

    std::int64_t totalPenalty_ = 0;
    std::int64_t routedPenalty_ = 0;
    std::vector<std::int64_t> routedPerVehicle_;
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAXIMIZE_VISITS_ASSIGNED_HPP
