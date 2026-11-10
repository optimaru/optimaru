#ifndef PDP_CONSTRAINTS_TRAVEL_TIME_COST_HPP
#define PDP_CONSTRAINTS_TRAVEL_TIME_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp {

// Soft, weight 1: travelTimeCost — each vehicle's driving seconds for the
// whole route, home -> nodes..., plus the leg back home when the vehicle
// returns to its depot. Raw cost per vehicle = those driving seconds; a
// vehicle that stays parked drives nothing and costs 0 (so does the pool).
struct TravelTimeCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TravelTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("travelTimeCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& plan, int unit,
                const std::vector<int>& members) const override {
        if (detail::isPool(plan, unit)) return 0.0;
        return static_cast<double>(detail::walkRoute(plan, unit, members, [](int, Seconds) {}));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_TRAVEL_TIME_COST_HPP
