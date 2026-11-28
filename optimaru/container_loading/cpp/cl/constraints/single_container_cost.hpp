#ifndef CL_CONSTRAINTS_SINGLE_CONTAINER_COST_HPP
#define CL_CONSTRAINTS_SINGLE_CONTAINER_COST_HPP

#include <cstdint>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: singleContainerCost -- a group that must ship in one
// container spread over several. Raw cost (global) = over the groups whose
// rule asks for one container (singleContainer), the containers holding a
// placed item of the group beyond the first. 0 = every such group stands in
// one container (or nowhere).
//
// groupSplitCost (soft) counts every group's spread by the lists; this one
// counts the placed items of the flagged groups only, so an item left out
// splits nothing.
struct SingleContainerCost final : detail::PlacedGroupConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SingleContainerCost(double weight = kDefaultWeight)
        : PlacedGroupConstraint("singleContainerCost", weight, kDefaultLevel) {}

    bool flagged(const ContainerLoadingProblem& problem, int group) const override {
        return problem.groupSingleContainer[static_cast<std::size_t>(group)] != 0;
    }
    std::int64_t groupCost(int /*listed*/, int /*placed*/, int holders) const override {
        return holders > 1 ? holders - 1 : 0;
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_SINGLE_CONTAINER_COST_HPP
