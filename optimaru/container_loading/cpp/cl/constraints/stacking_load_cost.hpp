#ifndef CL_CONSTRAINTS_STACKING_LOAD_COST_HPP
#define CL_CONSTRAINTS_STACKING_LOAD_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: stackingLoadCost -- weight resting on an item above its
// maxLoadOnTop. Raw cost per container = over its placed items that set a
// limit, the load on the item above that limit, each rounded up to a whole
// weight unit; 0 = no item carries more than it can (maxLoadOnTop 0 = nothing
// may stand on it).
//
// The load on an item is what the items standing on it pass down: each item
// passes its own weight plus its load to the items right under it, shared by
// contact area (stackingExcess in cl/score/packing.hpp).
struct StackingLoadCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit StackingLoadCost(double weight = kDefaultWeight)
        : PackedConstraint("stackingLoadCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.anyLoadLimit || members.empty()) return 0.0;
        return static_cast<double>(stackingExcess(problem, packing(problem, unit, members)));
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_STACKING_LOAD_COST_HPP
