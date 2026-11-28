#ifndef CL_CONSTRAINTS_SEGREGATION_COST_HPP
#define CL_CONSTRAINTS_SEGREGATION_COST_HPP

#include <cstddef>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: segregationCost -- items kept apart by a segregation rule
// that are not. Raw cost per container = the pairs of its placed items whose
// segregationClasses a rule keeps out of one container, plus the pairs whose
// classes must keep a distance and stand closer than it in the floor plane;
// 0 = every rule holds (segregationPairs in cl/score/packing.hpp).
//
// Scoped (scoped_units()) to every container when some item's class is named
// by a rule, and to none otherwise.
struct SegregationCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SegregationCost(double weight = kDefaultWeight)
        : PackedConstraint("segregationCost", weight, kDefaultLevel) {}

    void on_reset(const ContainerLoadingProblem& problem) override {
        PackedConstraint::on_reset(problem);
        scoped_.clear();
        if (problem.anySegregation) {
            for (std::size_t c = 0; c < problem.containers.size(); ++c)
                scoped_.push_back(static_cast<int>(c));
        }
    }
    const std::vector<int>* scoped_units() const override { return &scoped_; }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.anySegregation || members.empty()) return 0.0;
        return static_cast<double>(segregationPairs(problem, packing(problem, unit, members)));
    }

private:
    std::vector<int> scoped_;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_SEGREGATION_COST_HPP
