#ifndef CL_CONSTRAINTS_USED_LENGTH_COST_HPP
#define CL_CONSTRAINTS_USED_LENGTH_COST_HPP

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 0 (opt-in): usedLengthCost -- how far the load reaches from
// the closed end. Raw cost per container = the largest x + placed length of
// its placed items (the loading metres of a cargo space, in the caller's
// length unit); 0 for an empty container. Pick it with a weight to pack
// towards the closed end and keep the rest of the floor free.
struct UsedLengthCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UsedLengthCost(double weight = kDefaultWeight)
        : PackedConstraint("usedLengthCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        return static_cast<double>(packing(problem, unit, members).usedLength);
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_USED_LENGTH_COST_HPP
