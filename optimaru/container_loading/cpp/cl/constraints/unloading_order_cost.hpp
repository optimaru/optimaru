#ifndef CL_CONSTRAINTS_UNLOADING_ORDER_COST_HPP
#define CL_CONSTRAINTS_UNLOADING_ORDER_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: unloadingOrderCost -- an item that leaves later stands in
// the way of one that leaves earlier. Raw cost per container = the pairs
// (a, b) of its placed items with unloadOrder(a) < unloadOrder(b) where b
// blocks a: b is between a and the door (at or beyond a's far end along x,
// overlapping a in y and z) or above a (at or above a's top, overlapping a in
// x and y). 0 = every item can be taken out, towards the door, when its turn
// comes. Items without an unloadOrder are in nobody's way and wait for
// nobody.
struct UnloadingOrderCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit UnloadingOrderCost(double weight = kDefaultWeight)
        : PackedConstraint("unloadingOrderCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.anyUnloadOrder || members.empty()) return 0.0;
        return static_cast<double>(unloadingBlocks(problem, packing(problem, unit, members)));
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_UNLOADING_ORDER_COST_HPP
