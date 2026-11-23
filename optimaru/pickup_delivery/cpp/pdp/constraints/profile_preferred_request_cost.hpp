#ifndef PDP_CONSTRAINTS_PROFILE_PREFERRED_REQUEST_COST_HPP
#define PDP_CONSTRAINTS_PROFILE_PREFERRED_REQUEST_COST_HPP

#include "pdp/constraints/profile_request_table.hpp"

namespace pdp {

// Soft, weight 300: profilePreferredRequestCost -- a request that some
// vehicle's profile prefers (ProfileRequests::preferredRequestIndices) rides
// a vehicle whose profile does not.
//
// Raw cost per vehicle = the stops on its route whose request the entry of
// some vehicle prefers and its own entry does not, so such a request carried
// whole costs 2; 0 = every preferred request rides a vehicle that prefers it
// (or none is preferred). An unassigned request is no vehicle's:
// unassignedRequestsCost prices it. At weight 300 per stop against
// travelTimeCost's 1 per driving second, a vehicle that does not prefer a
// request takes it only when that saves more than ten minutes of driving.
//
// A plan that prefers requests needs this component in its constraints: the
// Python bindings refuse a selection that leaves it out
// (withAlwaysOnConstraints in constraints.hpp).
struct ProfilePreferredRequestCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 300.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ProfilePreferredRequestCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("profilePreferredRequestCost", weight, kDefaultLevel) {}

    void on_reset(const PdpRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        table_.build(plan, detail::ProfileRequestTable::Kind::Unmatched);
    }

    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (!table_.active() || detail::isPool(p, u)) return 0.0;
        return table_.count(u, m);
    }

    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_cost(const PdpRoutePlan& p, int u, const optimaru::ListPiece* pieces, int count,
                        double& out) const override {
        out = !table_.active() || detail::isPool(p, u) ? 0.0 : table_.count(u, pieces, count);
        return true;
    }

private:
    detail::ProfileRequestTable table_;
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PROFILE_PREFERRED_REQUEST_COST_HPP
