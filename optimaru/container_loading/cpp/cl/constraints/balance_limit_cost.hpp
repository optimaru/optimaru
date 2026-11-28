#ifndef CL_CONSTRAINTS_BALANCE_LIMIT_COST_HPP
#define CL_CONSTRAINTS_BALANCE_LIMIT_COST_HPP

#include <cstddef>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: balanceLimitCost -- a load's centre of gravity beyond the
// container's limits. Raw cost per container = how far its placed items'
// centre of gravity lies from the container's centre beyond maxOffsetLength
// (along x, in thousandths of half the length) plus beyond maxOffsetWidth
// (along y, in thousandths of half the width), each rounded up; 0 for a
// container without the limits, within them, or whose placed items weigh
// nothing. Scoped (scoped_units()) to the containers that set a limit.
struct BalanceLimitCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit BalanceLimitCost(double weight = kDefaultWeight)
        : PackedConstraint("balanceLimitCost", weight, kDefaultLevel) {}

    // Scoped to the containers that set a limit.
    void on_reset(const ContainerLoadingProblem& problem) override {
        PackedConstraint::on_reset(problem);
        scoped_.clear();
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            const Container& container = problem.containers[c];
            if (container.maxOffsetLength || container.maxOffsetWidth)
                scoped_.push_back(static_cast<int>(c));
        }
    }
    const std::vector<int>* scoped_units() const override { return &scoped_; }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        const Container& container = problem.containers[static_cast<std::size_t>(unit)];
        if ((!container.maxOffsetLength && !container.maxOffsetWidth) || members.empty())
            return 0.0;
        return static_cast<double>(balanceExcess(problem, unit, packing(problem, unit, members)));
    }

private:
    std::vector<int> scoped_;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_BALANCE_LIMIT_COST_HPP
