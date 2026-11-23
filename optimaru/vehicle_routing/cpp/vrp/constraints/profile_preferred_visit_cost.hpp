#ifndef VRP_CONSTRAINTS_PROFILE_PREFERRED_VISIT_COST_HPP
#define VRP_CONSTRAINTS_PROFILE_PREFERRED_VISIT_COST_HPP

#include "vrp/constraints/profile_visit_table.hpp"

namespace vrp {

// Soft, weight 600: profilePreferredVisitCost -- a visit that some vehicle's
// profile prefers (ProfileVisits::preferredVisitIndices) is served by a
// vehicle whose profile does not.
//
// Raw cost per vehicle = the visits on its route that the entry of some
// vehicle prefers and its own entry does not; 0 = every preferred visit
// rides a vehicle that prefers it (or none is preferred). An unassigned visit
// is no vehicle's: unassignedVisitsCost prices it. At weight 600 against
// travelTimeCost's 1 per driving second, a vehicle that does not prefer a
// visit takes it only when that saves more than ten minutes of driving.
//
// A plan that prefers visits needs this component in its constraints: the
// Python bindings refuse a selection that leaves it out
// (withAlwaysOnConstraints in constraints.hpp).
struct ProfilePreferredVisitCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 600.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ProfilePreferredVisitCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("profilePreferredVisitCost", weight, kDefaultLevel) {}

    void on_reset(const VehicleRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        table_.build(plan, detail::ProfileVisitTable::Kind::Unmatched);
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

#endif  // VRP_CONSTRAINTS_PROFILE_PREFERRED_VISIT_COST_HPP
