#ifndef VRP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP
#define VRP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Soft (default, weight 1): minimizeTravelTime -- each vehicle's driving time
// for the whole route, home -> visits..., plus the leg back home when the
// vehicle returns to its depot. A vehicle that stays parked drives nothing.
// Previewed in O(pieces): each piece's internal legs come from the shared
// distance sums, the legs between pieces and home are looked up.
struct MinimizeTravelTime final : detail::SummedUnitConstraint {
    explicit MinimizeTravelTime(double weight = 1.0)
        : SummedUnitConstraint("minimizeTravelTime", weight, /*hard=*/false) {}

    bool piecewiseCost(const VehicleRoutePlan& plan, int unit, const optimaru::ListPiece* pieces,
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

#endif  // VRP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP
