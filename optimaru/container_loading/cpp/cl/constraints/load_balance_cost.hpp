#ifndef CL_CONSTRAINTS_LOAD_BALANCE_COST_HPP
#define CL_CONSTRAINTS_LOAD_BALANCE_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 0 (opt-in): loadBalanceCost -- how far a container's load is
// from being centred. Raw cost per container = the distance of its placed
// items' centre of gravity from the container's centre in the x-y plane, in
// thousandths of the half-diagonal: 0 = centred, 1000 = in a corner. 0 for a
// container whose placed items weigh nothing.
struct LoadBalanceCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LoadBalanceCost(double weight = kDefaultWeight)
        : PackedConstraint("loadBalanceCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        return static_cast<double>(balancePerMille(problem, unit, packing(problem, unit, members)));
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_LOAD_BALANCE_COST_HPP
