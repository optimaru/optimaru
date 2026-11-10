#ifndef PDP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP
#define PDP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1: maxTravelTimeCost — driving seconds above the vehicle's
// Vehicle::maxTravelTime (the leg home counts only with returnToDepot). Raw
// cost per vehicle = max(0, driving seconds - maxTravelTime) while
// maxTravelTime > 0; 0 = within the limit, or no limit set (the pool list
// costs 0).
struct MaxTravelTimeCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxTravelTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxTravelTimeCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || p.vehicles[u].maxTravelTime <= 0)
            return 0;
        return static_cast<double>(
            std::max<Seconds>(0,
                              detail::routeMetrics(p, u, m).driving
                                  - p.vehicles[u].maxTravelTime));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MAX_TRAVEL_TIME_COST_HPP
