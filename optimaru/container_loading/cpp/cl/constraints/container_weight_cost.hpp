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
// container it was given to. The one exception is an unplaced optional item,
// which is out of the load: its weight does not count. Without optional items
// it needs no decode.
struct ContainerWeightCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ContainerWeightCost(double weight = kDefaultWeight)
        : PackedConstraint("containerWeightCost", weight, kDefaultLevel) {}

    // The raw cost, for the component and ScoreCalculator alike. `left` is
    // the weight of the list's unplaced optional items (0 without them).
    static std::int64_t excess(const ContainerLoadingProblem& problem, int unit,
                               const std::vector<int>& members, std::int64_t left = 0) {
        const Container& container = problem.containers[static_cast<std::size_t>(unit)];
        if (!container.maxWeight) return 0;
        std::int64_t weight = -left;
        for (int item : members) weight += problem.items[static_cast<std::size_t>(item)].weight;
        return weight > *container.maxWeight ? weight - *container.maxWeight : 0;
    }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.anyOptional || members.empty())
            return static_cast<double>(excess(problem, unit, members));
        const Container& container = problem.containers[static_cast<std::size_t>(unit)];
        if (!container.maxWeight) return 0.0;
        return static_cast<double>(
                excess(problem, unit, members,
                       unplacedOptionalWeight(problem, packing(problem, unit, members))));
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_CONTAINER_WEIGHT_COST_HPP
