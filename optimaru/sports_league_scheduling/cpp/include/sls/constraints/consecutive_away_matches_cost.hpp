#ifndef SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_COST_HPP
#define SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls {

// Hard, weight 1: consecutiveAwayMatchesCost — the away-side mirror of
// consecutiveHomeMatchesCost, and the one that actually keeps teams from
// living on the road for a week. Raw cost (one global total) = summed over
// teams, the whole length of every run of 4 or more consecutive away round
// indices. 0 = no team has an away run of 4 or more.
struct ConsecutiveAwayMatchesCost final : TeamSideRunConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ConsecutiveAwayMatchesCost(double weight = kDefaultWeight)
        : TeamSideRunConstraint("consecutiveAwayMatchesCost", weight, kDefaultLevel) {}
    int teamOf(const Match& match) const override { return match.awayTeam; }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_COST_HPP
