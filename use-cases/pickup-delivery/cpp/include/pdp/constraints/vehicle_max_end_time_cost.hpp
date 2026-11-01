#ifndef PDP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP
#define PDP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1: vehicleMaxEndTimeCost — the route end (the last stop's
// departure, or the return home with returnToDepot) after the vehicle's
// Vehicle::maxEndTime. Raw cost per vehicle = max(0, endTime - maxEndTime)
// seconds while maxEndTime > 0; 0 = back in time, or no limit set (the pool
// list costs 0). A stop's own time window is maxEndTimeCost.
struct VehicleMaxEndTimeCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VehicleMaxEndTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("vehicleMaxEndTimeCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || p.vehicles[u].maxEndTime <= 0)
            return 0;
        return static_cast<double>(std::max<Seconds>(
            0,
            detail::routeMetrics(p, u, m).endTime - p.vehicles[u].maxEndTime));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_VEHICLE_MAX_END_TIME_COST_HPP
