#ifndef PDP_CONSTRAINTS_ROUTE_LIMITS_HPP
#define PDP_CONSTRAINTS_ROUTE_LIMITS_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
struct MaxTravelTime final : PdpUnitConstraint
{
    MaxTravelTime() : PdpUnitConstraint("maxTravelTime", 1.0, true) {}
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
struct MaxRouteDuration final : PdpUnitConstraint
{
    MaxRouteDuration() : PdpUnitConstraint("maxRouteDuration", 1.0, true) {}
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || p.vehicles[u].maxRouteDuration <= 0)
            return 0;
        const auto x = detail::routeMetrics(p, u, m);
        return static_cast<double>(
            std::max<Seconds>(0,
                              x.endTime - p.vehicles[u].departureTime
                                  - p.vehicles[u].maxRouteDuration));
    }
};
struct VehicleEndTime final : PdpUnitConstraint
{
    VehicleEndTime() : PdpUnitConstraint("vehicleEndTime", 1.0, true) {}
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
struct MaxStops final : PdpUnitConstraint
{
    MaxStops() : PdpUnitConstraint("maxStops", 1.0, true) {}
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || p.vehicles[u].maxStops <= 0)
            return 0;
        return static_cast<double>(
            std::max(0, static_cast<int>(m.size()) - p.vehicles[u].maxStops));
    }
};
}  // namespace pdp
#endif
