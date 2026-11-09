#ifndef VRP_CONSTRAINTS_TRAVEL_TIME_COST_HPP
#define VRP_CONSTRAINTS_TRAVEL_TIME_COST_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Soft, weight 1: travelTimeCost -- each vehicle's driving time for the whole
// route, home -> visits..., plus the leg back home when the vehicle returns
// to its depot. Raw cost per vehicle = its driving seconds; 0 = a vehicle
// that stays parked (and the pool list). Previewed in O(pieces): each
// piece's internal legs come from the shared distance sums, the legs between
// pieces and home are looked up.
struct TravelTimeCost final : detail::SummedUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TravelTimeCost(double weight = kDefaultWeight)
        : SummedUnitConstraint("travelTimeCost", weight, kDefaultLevel) {}

    bool piecewise_cost(const VehicleRoutePlan& plan, int unit, const optimaru::ListPiece* pieces,
                       int count, double& out) const override {
        if (detail::isPool(plan, unit)) {
            out = 0.0;
            return true;
        }
        const detail::RouteSumsContext::PieceWalk walk = sums_->walk(unit, pieces, count, /*withMinEnd=*/false);
        if (!walk.any) {
            out = 0.0;
            return true;
        }
        out = static_cast<double>(
                walk.drive + plan.vehicles[unit].homeLeg(plan.drivingSeconds, walk.lastLocation));
        return true;
    }

    double cost(const VehicleRoutePlan& plan, int unit,
                const std::vector<int>& members) const override {
        if (detail::isPool(plan, unit)) return 0.0;  // nobody drives the pool
        return static_cast<double>(
                detail::walkRoute(plan, unit, members, [](int, Seconds) {}));
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_TRAVEL_TIME_COST_HPP
