#ifndef CL_CONSTRAINTS_USED_CONTAINER_COST_HPP
#define CL_CONSTRAINTS_USED_CONTAINER_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 1: usedContainerCost -- what the containers in use cost. Raw
// cost per container = its `cost` when it holds a placed item, else 0. With
// every cost at 1 (the default) the total is the number of containers used;
// with a cost per carton type it is the price of the cartons chosen.
//
// A container whose list holds only items that found no place is not in use.
struct UsedContainerCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UsedContainerCost(double weight = kDefaultWeight)
        : PackedConstraint("usedContainerCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        return packing(problem, unit, members).boxes.empty()
                       ? 0.0
                       : problem.containers[static_cast<std::size_t>(unit)].cost;
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_USED_CONTAINER_COST_HPP
