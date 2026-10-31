#ifndef VRP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP
#define VRP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1: maxTravelTimeCost -- driving seconds above the vehicle's
// Vehicle::maxTravelTime (the leg home counts only with returnToDepot). Raw
// cost per vehicle = max(0, driving seconds - maxTravelTime) while
// maxTravelTime > 0; 0 = within the limit, or no limit set (the pool list
// costs 0).
struct MaxTravelTimeCost final : VrpUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxTravelTimeCost(double weight = kDefaultWeight)
        : VrpUnitConstraint("maxTravelTimeCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxTravelTime <= 0) return 0;
        return static_cast<double>(std::max<Seconds>(0,
            detail::routeMetrics(p, u, m).driving - p.vehicles[u].maxTravelTime));
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP
