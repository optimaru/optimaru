#ifndef BS_CONSTRAINTS_BOX_COST_HPP
#define BS_CONSTRAINTS_BOX_COST_HPP

#include <cstdint>

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, weight 1: boxCost -- what the boxes shipped cost. Raw cost per box =
// over its parcels, quantity * the box's cost, computed as (shipments in the
// box) * cost; 0 for a box without a cost or without parcels.
//
// The weight is what one unit of cost is worth in units of void volume: the
// two share the soft level, so a cost in a currency next to a void in mm3
// needs a weight that says how much air a unit of money buys.
struct BoxCost final : BsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit BoxCost(double weight = kDefaultWeight)
        : BsUnitConstraint("boxCost", weight, kDefaultLevel) {}
    void on_reset(const BoxSizingProblem& problem) override { detail::requirePrepared(problem); }
    // The shipments first, as a whole number, then one product: the double
    // ScoreCalculator::score computes.
    double cost(const BoxSizingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        std::int64_t quantity = 0;
        for (int parcel : members) quantity += problem.parcels[parcel].quantity;
        return static_cast<double>(quantity) * problem.boxes[unit].cost;
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_BOX_COST_HPP
