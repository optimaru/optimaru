#ifndef BS_CONSTRAINTS_VOID_VOLUME_COST_HPP
#define BS_CONSTRAINTS_VOID_VOLUME_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, weight 1: voidVolumeCost -- the objective, the air shipped. Raw cost
// per box = over its parcels, quantity * (box volume - parcel volume); 0 =
// every parcel of the box fills it. A parcel larger than its box (it does not
// fit: parcelFitCost) counts 0, so the cost is never negative.
struct VoidVolumeCost final : detail::PerParcelConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit VoidVolumeCost(double weight = kDefaultWeight)
        : PerParcelConstraint("voidVolumeCost", weight, kDefaultLevel) {}
    double term(const BoxSizingProblem& problem, int parcel, int box) const override {
        return static_cast<double>(problem.voidOf(parcel, box));
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_VOID_VOLUME_COST_HPP
