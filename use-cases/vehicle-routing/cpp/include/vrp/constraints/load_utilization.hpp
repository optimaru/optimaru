#ifndef VRP_CONSTRAINTS_LOAD_UTILIZATION_HPP
#define VRP_CONSTRAINTS_LOAD_UTILIZATION_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {
struct MinCapacityUtilization final : VrpUnitConstraint {
    MinCapacityUtilization() : VrpUnitConstraint("minCapacityUtilization", 1.0, false) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || m.empty() || p.vehicles[u].minLoadRatio <= 0) return 0;
        std::int64_t load = 0;
        for (int i : m) load += p.visits[i].demand;
        return std::max(0.0,
            p.vehicles[u].capacity * p.vehicles[u].minLoadRatio - load);
    }
};
}  // namespace vrp
#endif
