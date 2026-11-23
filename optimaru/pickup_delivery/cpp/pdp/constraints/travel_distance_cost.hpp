#ifndef PDP_CONSTRAINTS_TRAVEL_DISTANCE_COST_HPP
#define PDP_CONSTRAINTS_TRAVEL_DISTANCE_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp {

// Soft, weight 0 (opt-in): travelDistanceCost — each vehicle's driving
// meters for the whole route, start depot -> stops..., plus the leg to the
// end depot when the route has one, read from the distance matrix of the
// vehicle's profile. Raw cost per vehicle = its driving meters; 0 = a vehicle
// that stays parked (and the pool list). Give it a weight to price distance
// beside or instead of travelTimeCost.
struct TravelDistanceCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TravelDistanceCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("travelDistanceCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        return static_cast<double>(detail::routeMeters(p, u, m));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_TRAVEL_DISTANCE_COST_HPP
