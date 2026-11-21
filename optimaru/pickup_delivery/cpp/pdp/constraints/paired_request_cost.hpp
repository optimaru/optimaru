#ifndef PDP_CONSTRAINTS_PAIRED_REQUEST_COST_HPP
#define PDP_CONSTRAINTS_PAIRED_REQUEST_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp {

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty): pairedRequestCost — a
// request with one node routed and the other not. Half a delivery is worse
// than none: the goods are either picked up and never delivered, or delivered
// from nowhere. Raw cost (global) = the number of half-routed requests, so
// 0 = every request is routed whole or not at all.
//
// The CP-SAT reference enforces pairing structurally (its plugin refuses
// level='soft'); the engine keeps it flippable.
struct PairedRequestCost final : RequestCouplingConstraint {
    static constexpr double kDefaultWeight = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit PairedRequestCost(double weight = kDefaultWeight)
        : RequestCouplingConstraint("pairedRequestCost", weight, kDefaultLevel) {}
    std::int64_t requestCost(const PdpRoutePlan& /*plan*/, const Delivery& /*delivery*/,
                             int pickupUnit, int /*pickupPosition*/, int dropoffUnit,
                             int /*dropoffPosition*/) const override {
        return (pickupUnit >= 0) != (dropoffUnit >= 0) ? 1 : 0;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PAIRED_REQUEST_COST_HPP
