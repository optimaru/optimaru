#ifndef CL_CONSTRAINTS_STACKING_RULE_COST_HPP
#define CL_CONSTRAINTS_STACKING_RULE_COST_HPP

#include <cstddef>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Hard, weight 1: stackingRuleCost -- items standing on each other against a
// stacking rule, and columns taller than an item's maxStackLayers. Raw cost
// per container = the pairs (a, b) of placed items where a stands on b (its
// base on b's top, overlapping it in the floor plane) and a rule forbids a's
// stackGroup on b's, plus, over the placed items that set maxStackLayers, the
// layers the tallest column through the item has beyond its limit; 0 = every
// rule and limit holds.
//
// A column through an item is the item, the items under it down to the floor
// and the tallest chain standing on it (stackingViolations in
// cl/score/packing.hpp). Scoped (scoped_units()) to every container when some
// item sets maxStackLayers or a stackGroup a rule names, and to none
// otherwise, so a problem without them pays nothing for it per move.
struct StackingRuleCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit StackingRuleCost(double weight = kDefaultWeight)
        : PackedConstraint("stackingRuleCost", weight, kDefaultLevel) {}

    // Scoped to every container when an item can break a rule or a layer
    // limit, else to none: a problem without them never asks it.
    void on_reset(const ContainerLoadingProblem& problem) override {
        PackedConstraint::on_reset(problem);
        scoped_.clear();
        if (problem.anyStackRule || problem.anyLayerLimit) {
            for (std::size_t c = 0; c < problem.containers.size(); ++c)
                scoped_.push_back(static_cast<int>(c));
        }
    }
    const std::vector<int>* scoped_units() const override { return &scoped_; }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if ((!problem.anyStackRule && !problem.anyLayerLimit) || members.empty()) return 0.0;
        return static_cast<double>(stackingViolations(problem, packing(problem, unit, members)));
    }

private:
    std::vector<int> scoped_;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_STACKING_RULE_COST_HPP
