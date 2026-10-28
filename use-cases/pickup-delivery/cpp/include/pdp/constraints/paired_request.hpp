#ifndef PDP_CONSTRAINTS_PAIRED_REQUEST_HPP
#define PDP_CONSTRAINTS_PAIRED_REQUEST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard (default, weight kCouplingPenalty): pairedRequest — a request with one
// node routed and the other not. Half a delivery is worse than none: the goods
// are either picked up and never delivered, or delivered from nowhere.
struct PairedRequest final : RequestCouplingConstraint
{
    explicit PairedRequest(
        double weight = static_cast<double>(ScoreCalculator::kCouplingPenalty))
        : RequestCouplingConstraint("pairedRequest", weight, /*hard=*/true)
    {
    }
    std::int64_t requestCost(const PdpRoutePlan & /*plan*/,
                             const Delivery & /*delivery*/,
                             int pickupUnit,
                             int /*pickupPosition*/,
                             int dropoffUnit,
                             int /*dropoffPosition*/) const override
    {
        return (pickupUnit >= 0) != (dropoffUnit >= 0) ? 1 : 0;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PAIRED_REQUEST_HPP
