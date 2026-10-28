#ifndef PDP_CONSTRAINTS_ROUTE_COSTS_HPP
#define PDP_CONSTRAINTS_ROUTE_COSTS_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
struct MinimizeRouteDuration final : PdpUnitConstraint
{
    MinimizeRouteDuration()
        : PdpUnitConstraint("minimizeRouteDuration", 0.0, false)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || m.empty())
            return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).endTime
                                   - p.vehicles[u].departureTime);
    }
};
struct MinimizeLatency final : PdpUnitConstraint
{
    MinimizeLatency() : PdpUnitConstraint("minimizeLatency", 0.0, false) {}
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u))
            return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).latency);
    }
};
struct VehicleFixedCost final : PdpUnitConstraint
{
    VehicleFixedCost() : PdpUnitConstraint("vehicleFixedCost", 1.0, false) {}
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        return detail::isPool(p, u) || m.empty() ? 0.0
                                                 : p.vehicles[u].fixedCost;
    }
};
}  // namespace pdp
#endif
