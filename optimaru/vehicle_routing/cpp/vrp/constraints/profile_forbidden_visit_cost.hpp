#ifndef VRP_CONSTRAINTS_PROFILE_FORBIDDEN_VISIT_COST_HPP
#define VRP_CONSTRAINTS_PROFILE_FORBIDDEN_VISIT_COST_HPP

#include "vrp/constraints/profile_visit_table.hpp"

namespace vrp {

// Hard, weight 1000: profileForbiddenVisitCost -- a vehicle serves a visit
// its profile may not serve (ProfileVisits::forbiddenVisitIndices): a
// technician without the skill a job needs, a truck too large for a street.
//
// Raw cost per vehicle = the visits on its route that its entry forbids; 0 =
// every vehicle serves only what it may (or nothing is forbidden). The
// weight is far above a typical lateness or overload, and leaving the visit
// unassigned costs only medium, so a visit that no vehicle may serve waits.
//
// Every solve scores it: the Python bindings add it to a constraint
// selection that leaves it out (withAlwaysOnConstraints in constraints.hpp), and
// the operators that place visits from the plan (the group operators) skip
// a vehicle that forbids one of the visits they place.
struct ProfileForbiddenVisitCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ProfileForbiddenVisitCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("profileForbiddenVisitCost", weight, kDefaultLevel) {}

    void on_reset(const VehicleRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        table_.build(plan, detail::ProfileVisitTable::Kind::Forbidden);
    }

    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (!table_.active() || detail::isPool(p, u)) return 0.0;
        return table_.count(u, m);
    }

    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_cost(const VehicleRoutePlan& p, int u, const optimaru::ListPiece* pieces,
                        int count, double& out) const override {
        out = !table_.active() || detail::isPool(p, u) ? 0.0 : table_.count(u, pieces, count);
        return true;
    }

private:
    detail::ProfileVisitTable table_;
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_PROFILE_FORBIDDEN_VISIT_COST_HPP
