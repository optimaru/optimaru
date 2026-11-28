#ifndef BS_CONSTRAINTS_BOX_WEIGHT_COST_HPP
#define BS_CONSTRAINTS_BOX_WEIGHT_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Hard, weight 1: boxWeightCost -- a parcel may not weigh more than its box
// carries. Raw cost per box = over its parcels, the weight above the box's
// maxWeight; 0 = none is too heavy, and always 0 for a box without a
// maxWeight. A parcel is one shipment in one box, so the weights of a box's
// parcels are not added up against the limit.
struct BoxWeightCost final : detail::PerParcelConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit BoxWeightCost(double weight = kDefaultWeight)
        : PerParcelConstraint("boxWeightCost", weight, kDefaultLevel) {}
    double term(const BoxSizingProblem& problem, int parcel, int box) const override {
        return static_cast<double>(problem.weightExcess(parcel, box));
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_BOX_WEIGHT_COST_HPP
