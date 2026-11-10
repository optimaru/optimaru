#ifndef VRP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP
#define VRP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1: vehicleMaxEndTimeCost -- how far the route ends after
// Vehicle::maxEndTime (the vehicle's own deadline; a visit's window is
// maxEndTimeCost). Raw cost per vehicle = max(0, route end time -
// maxEndTime) seconds while maxEndTime > 0, where an empty route ends at its
// departure time; 0 = back in time, or no limit set (the pool list costs 0).
struct VehicleMaxEndTimeCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VehicleMaxEndTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("vehicleMaxEndTimeCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxEndTime <= 0) return 0;
        return static_cast<double>(std::max<Seconds>(
                0, detail::routeMetrics(p, u, m).endTime - p.vehicles[u].maxEndTime));
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP
