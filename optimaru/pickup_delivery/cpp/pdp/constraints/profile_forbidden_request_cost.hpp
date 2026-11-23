#ifndef PDP_CONSTRAINTS_PROFILE_FORBIDDEN_REQUEST_COST_HPP
#define PDP_CONSTRAINTS_PROFILE_FORBIDDEN_REQUEST_COST_HPP

#include "pdp/constraints/profile_request_table.hpp"

namespace pdp {

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty):
// profileForbiddenRequestCost -- a vehicle carries a request its profile may
// not carry (ProfileRequests::forbiddenRequestIndices): a load the vehicle
// cannot take, a driver without the permit a job needs.
//
// Raw cost per vehicle = the stops on its route whose request its entry
// forbids, so a forbidden request carried whole costs 2 (as maxStops counts
// a request twice) and each of its stops counts until both are off. 0 =
// every vehicle carries only what it may (or nothing is forbidden). Leaving
// the request unassigned costs only medium, so one that no vehicle may carry
// waits.
//
// Every solve scores it hard: the Python bindings add it at its defaults to a
// selection that holds no hard instance of it at a weight above 0
// (addAlwaysOnConstraints in constraints.hpp),
// and the request-level operators never offer a slot on a vehicle that
// forbids the request (RouteTimeline::improve).
struct ProfileForbiddenRequestCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ProfileForbiddenRequestCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("profileForbiddenRequestCost", weight, kDefaultLevel) {}

    void on_reset(const PdpRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        table_.build(plan, detail::ProfileRequestTable::Kind::Forbidden);
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

#endif  // PDP_CONSTRAINTS_PROFILE_FORBIDDEN_REQUEST_COST_HPP
