#ifndef CL_CONSTRAINTS_CONTAINER_WEIGHT_COST_HPP
#define CL_CONSTRAINTS_CONTAINER_WEIGHT_COST_HPP

#include <cstdint>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: containerWeightCost -- the weight of a container's list
// above the container's maxWeight. Raw cost per container = max(0, the summed
// weight of its items - maxWeight), in the caller's weight unit; 0 for a
// container without a limit.
//
// It counts the whole list, placed or not: an item that found no place is
// already an unplacedItemCost, and what it weighs still belongs to the
// container it was given to. It needs no decode.
struct ContainerWeightCost final : ClUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ContainerWeightCost(double weight = kDefaultWeight)
        : ClUnitConstraint("containerWeightCost", weight, kDefaultLevel) {}

    // The raw cost, for the component and ScoreCalculator alike.
    static std::int64_t excess(const ContainerLoadingProblem& problem, int unit,
                               const std::vector<int>& members) {
        const Container& container = problem.containers[static_cast<std::size_t>(unit)];
        if (!container.maxWeight) return 0;
        std::int64_t weight = 0;
        for (int item : members) weight += problem.items[static_cast<std::size_t>(item)].weight;
        return weight > *container.maxWeight ? weight - *container.maxWeight : 0;
    }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        return static_cast<double>(excess(problem, unit, members));
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_CONTAINER_WEIGHT_COST_HPP
