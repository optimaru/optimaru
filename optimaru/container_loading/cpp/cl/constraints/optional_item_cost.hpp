#ifndef CL_CONSTRAINTS_OPTIONAL_ITEM_COST_HPP
#define CL_CONSTRAINTS_OPTIONAL_ITEM_COST_HPP

#include <cstddef>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 1: optionalItemCost -- optional items left out. Raw cost per
// container = over the optional items of its list that found no place, their
// priorities (1 each by default); 0 = every optional item is loaded.
//
// An optional item counts here and not in unplacedItemCost: it is loaded
// where there is room, and what is left out costs as much as the weight
// says. Scoped (scoped_units()) to every container when some item is
// optional, and to none otherwise.
struct OptionalItemCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit OptionalItemCost(double weight = kDefaultWeight)
        : PackedConstraint("optionalItemCost", weight, kDefaultLevel) {}

    void on_reset(const ContainerLoadingProblem& problem) override {
        PackedConstraint::on_reset(problem);
        scoped_.clear();
        if (problem.anyOptional) {
            for (std::size_t c = 0; c < problem.containers.size(); ++c)
                scoped_.push_back(static_cast<int>(c));
        }
    }
    const std::vector<int>* scoped_units() const override { return &scoped_; }

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.anyOptional || members.empty()) return 0.0;
        return static_cast<double>(optionalCost(problem, packing(problem, unit, members)));
    }

private:
    std::vector<int> scoped_;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_OPTIONAL_ITEM_COST_HPP
