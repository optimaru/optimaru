#ifndef PDP_CONSTRAINTS_PINNED_STOPS_HPP
#define PDP_CONSTRAINTS_PINNED_STOPS_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
// Hard (default, weight kPinnedPenalty): pinnedStops — a route must start with
// its vehicle's pinnedNodeIndices, in that order. A move that disturbs them, or
// slips a stop in among them, simply scores worse, so no move operator has to
// know about pinning. The weight keeps that true when the route also breaks
// other hard rules: a solve starts with its pinned stops in place, and nothing
// it could gain elsewhere makes a best solution give them up. The rest of the
// route stays free to change. Pinning a pickup also keeps its request on the
// vehicle, through sameVehicle.
struct PinnedStops final : PdpUnitConstraint
{
    PinnedStops()
        : PdpUnitConstraint("pinnedStops",
                            static_cast<double>(ScoreCalculator::kPinnedPenalty),
                            /*hard=*/true)
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
#endif
