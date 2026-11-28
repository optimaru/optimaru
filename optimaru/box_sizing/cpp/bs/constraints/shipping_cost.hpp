#ifndef BS_CONSTRAINTS_SHIPPING_COST_HPP
#define BS_CONSTRAINTS_SHIPPING_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, weight 0 (opt-in): shippingCost -- what the parcels cost to ship.
// Raw cost per box = over its parcels, quantity * the price of a shipment's
// billed weight in the rate table of the parcel's zone. The billed weight is
// the parcel's weight, or the box's dimensional weight (ceil(box volume /
// dimDivisor)) when that is more: a large box bills a light parcel by its
// size. 0 without rate tables.
//
// It shares the soft level with the void volume, so its weight says how much
// volume one unit of money is worth; to minimise the shipping cost alone,
// pick it with the void volume and the box cost left out.
struct ShippingCost final : detail::PerParcelConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ShippingCost(double weight = kDefaultWeight)
        : PerParcelConstraint("shippingCost", weight, kDefaultLevel) {}
    double term(const BoxSizingProblem& problem, int parcel, int box) const override {
        return static_cast<double>(problem.parcels[parcel].quantity) *
               problem.shippingPrice(parcel, box);
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_SHIPPING_COST_HPP
