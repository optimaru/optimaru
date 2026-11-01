#ifndef PDP_CONSTRAINTS_LATENCY_COST_HPP
#define PDP_CONSTRAINTS_LATENCY_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft, weight 0 (opt-in, default_enabled = false): latencyCost — how long
// the stops wait for their vehicle. Raw cost per vehicle = the sum over its
// stops of max(0, arrival - departureTime) in seconds; 0 for an unused
// vehicle (and for the pool list).
struct LatencyCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LatencyCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("latencyCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u))
            return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).latency);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_LATENCY_COST_HPP
