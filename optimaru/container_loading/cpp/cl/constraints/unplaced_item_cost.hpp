#ifndef CL_CONSTRAINTS_UNPLACED_ITEM_COST_HPP
#define CL_CONSTRAINTS_UNPLACED_ITEM_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: unplacedItemCost -- the items of a container's list for
// which the decoder found no place: nowhere inside the container where the
// item, in one of its allowed orientations, overlaps nothing and has enough
// of its base supported. Raw cost per container = the number of such items,
// so 0 = everything in the list stands somewhere.
//
// An unplaced item stays in its list. The search moves it to another
// container or to another place in the order, where the decoder may find
// room for it.
struct UnplacedItemCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit UnplacedItemCost(double weight = kDefaultWeight)
        : PackedConstraint("unplacedItemCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        return static_cast<double>(packing(problem, unit, members).unplaced());
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_UNPLACED_ITEM_COST_HPP
