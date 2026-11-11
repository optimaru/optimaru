#ifndef VRP_CONSTRAINTS_VISIT_ORDER_COST_HPP
#define VRP_CONSTRAINTS_VISIT_ORDER_COST_HPP

#include <cstdint>
#include <vector>

#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1000: visitOrderCost -- a visit served before one it has to
// come after (Visit::afterVisitIndices). The order is among the visits of one
// group, so it is an order along one route.
//
// Raw cost per vehicle = the pairs (earlier, later) on its route that are
// served the wrong way round; 0 = every such pair on the route is in order
// (the pool list costs 0). A pair whose two visits ride different vehicles is
// not counted here: visitGroupCost prices the group being apart.
struct VisitOrderCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VisitOrderCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("visitOrderCost", weight, kDefaultLevel) {}

    void on_reset(const VehicleRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        ordered_ = false;
        for (const Visit& visit : plan.visits)
            ordered_ = ordered_ || !visit.afterVisitIndices.empty();
        position_.assign(ordered_ ? plan.visits.size() : 0, 0);
        stamp_.assign(position_.size(), 0);
        epoch_ = 0;
    }

    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (!ordered_ || detail::isPool(p, u)) return 0;
        ++epoch_;
        for (std::size_t i = 0; i < m.size(); ++i) {
            const auto visit = static_cast<std::size_t>(m[i]);
            position_[visit] = static_cast<int>(i);
            stamp_[visit] = epoch_;
        }
        std::int64_t reversed = 0;
        for (std::size_t i = 0; i < m.size(); ++i) {
            for (int earlier : p.visits[static_cast<std::size_t>(m[i])].afterVisitIndices) {
                const auto e = static_cast<std::size_t>(earlier);
                if (stamp_[e] == epoch_ && position_[e] > static_cast<int>(i)) ++reversed;
            }
        }
        return static_cast<double>(reversed);
    }

    // A plan with no order costs nothing on any candidate route, so its
    // previews need no route written out. With an order the candidate route
    // is costed as it is (cost() above).
    bool supports_piecewise_preview() const override { return !ordered_; }
    bool piecewise_cost(const VehicleRoutePlan& /*plan*/, int /*unit*/,
                        const optimaru::ListPiece* /*pieces*/, int /*count*/,
                        double& out) const override {
        if (ordered_) return false;
        out = 0.0;
        return true;
    }

private:
    bool ordered_ = false;  // some visit names one it comes after
    // Where each visit of the route being costed sits, valid while its stamp
    // is the current epoch.
    mutable std::vector<int> position_;
    mutable std::vector<std::uint64_t> stamp_;
    mutable std::uint64_t epoch_ = 0;
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_VISIT_ORDER_COST_HPP
