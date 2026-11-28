#ifndef CL_CONSTRAINTS_AXLE_LOAD_COST_HPP
#define CL_CONSTRAINTS_AXLE_LOAD_COST_HPP

#include <cstddef>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: axleLoadCost -- axle groups loaded above their limit. Raw
// cost per container with two axle groups = over its stages (departure, and
// after each stop's items are unloaded), the largest sum of what the groups
// carry above their maxLoad, each group's rounded up to a whole weight unit; 0 = no group
// is overloaded at any stage, or the container has no axle groups or holds no
// placed item.
//
// What a group carries follows from the lever rule (axleStages in
// cl/score/packing.hpp). Scoped (scoped_units()) to the containers with axle
// groups.
struct AxleLoadCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit AxleLoadCost(double weight = kDefaultWeight)
        : PackedConstraint("axleLoadCost", weight, kDefaultLevel) {}

    void on_reset(const ContainerLoadingProblem& problem) override {
        PackedConstraint::on_reset(problem);
        scoped_.clear();
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            if (!problem.containers[c].axleGroups.empty()) scoped_.push_back(static_cast<int>(c));
        }
    }
    const std::vector<int>* scoped_units() const override { return &scoped_; }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (problem.containers[static_cast<std::size_t>(unit)].axleGroups.empty() ||
            members.empty())
            return 0.0;
        return static_cast<double>(axleExcess(problem, unit, packing(problem, unit, members)));
    }

private:
    std::vector<int> scoped_;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_AXLE_LOAD_COST_HPP
