#ifndef BS_CONSTRAINTS_PARCEL_FIT_COST_HPP
#define BS_CONSTRAINTS_PARCEL_FIT_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Hard, weight 1: parcelFitCost -- a parcel must fit its box in some
// rotation: its three sides, sorted, each at most the box's sorted sides. Raw
// cost per box = over its parcels, the length by which the parcel exceeds the
// box, summed over the three sorted sides; 0 = every parcel of the box fits.
// The quantity does not multiply it: the cost says how far a size is from
// fitting, not how many shipments had it.
struct ParcelFitCost final : detail::PerParcelConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ParcelFitCost(double weight = kDefaultWeight)
        : PerParcelConstraint("parcelFitCost", weight, kDefaultLevel) {}
    double term(const BoxSizingProblem& problem, int parcel, int box) const override {
        return static_cast<double>(problem.fitExcess(parcel, box));
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_PARCEL_FIT_COST_HPP
