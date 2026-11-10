#ifndef PDP_CONSTRAINTS_PINNED_STOPS_COST_HPP
#define PDP_CONSTRAINTS_PINNED_STOPS_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1e12 (ScoreCalculator::kPinnedPenalty): pinnedStopsCost — a
// route must start with its vehicle's pinnedNodeIndices, in that order. A move
// that disturbs them, or slips a stop in among them, simply scores worse, so no
// move operator has to know about pinning. The weight keeps that true when the
// route also breaks other hard rules: a solve starts with its pinned stops in
// place, and short of 1e12 minutes of lateness nothing it could gain elsewhere
// makes a best solution give them up. The rest of the route stays free to
// change. Pinning a pickup also keeps its request on the vehicle, through
// sameVehicleCost.
//
// Raw cost per vehicle = the number of positions i < pinned count where the
// route's i-th stop is not the i-th pinned stop; 0 = the pinned prefix is in
// place, or nothing is pinned (the pool list costs 0).
struct PinnedStopsCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight
        = static_cast<double>(ScoreCalculator::kPinnedPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit PinnedStopsCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("pinnedStopsCost", weight, kDefaultLevel)
    {
    }

    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u))
            return 0;
        const std::vector<int> &pinned = p.vehicles[u].pinnedNodeIndices;
        double misplaced = 0;
        for (std::size_t i = 0; i < pinned.size(); ++i)
        {
            if (i >= m.size() || m[i] != pinned[i])
                misplaced += 1;
        }
        return misplaced;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PINNED_STOPS_COST_HPP
