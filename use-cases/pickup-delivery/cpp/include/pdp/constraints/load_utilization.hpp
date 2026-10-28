#ifndef PDP_CONSTRAINTS_LOAD_UTILIZATION_HPP
#define PDP_CONSTRAINTS_LOAD_UTILIZATION_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
struct MinCapacityUtilization final : PdpUnitConstraint
{
    MinCapacityUtilization()
        : PdpUnitConstraint("minCapacityUtilization", 1.0, false)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || m.empty()
            || p.vehicles[u].minLoadRatio <= 0)
            return 0;
        std::int64_t delivered = 0;
        for (int i : m)
        {
            const Node &n = p.nodes[i];
            if (n.pickup)
                delivered += p.deliveries[n.deliveryIndex].demand;
        }
        return std::max(0.0,
                        p.vehicles[u].capacity * p.vehicles[u].minLoadRatio
                            - delivered);
    }
};
}  // namespace pdp
#endif
