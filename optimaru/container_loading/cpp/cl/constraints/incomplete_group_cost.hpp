#ifndef CL_CONSTRAINTS_INCOMPLETE_GROUP_COST_HPP
#define CL_CONSTRAINTS_INCOMPLETE_GROUP_COST_HPP

#include <algorithm>
#include <cstdint>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: incompleteGroupCost -- a group loaded in part. Raw cost
// (global) = over the groups whose rule asks for all or none (allOrNone),
// the smaller of the items placed and the items left out: what it takes to
// load the group whole or not at all. 0 = every such group is loaded whole or
// not at all.
//
// It counts the items the lists hold: an item that found no place is left
// out. All or none is a choice only for optional items; a group of others is
// already all, through unplacedItemCost.
struct IncompleteGroupCost final : detail::PlacedGroupConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit IncompleteGroupCost(double weight = kDefaultWeight)
        : PlacedGroupConstraint("incompleteGroupCost", weight, kDefaultLevel) {}

    bool flagged(const ContainerLoadingProblem& problem, int group) const override {
        return problem.groupAllOrNone[static_cast<std::size_t>(group)] != 0;
    }
    std::int64_t groupCost(int listed, int placed, int /*holders*/) const override {
        return std::min(placed, listed - placed);
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_INCOMPLETE_GROUP_COST_HPP
