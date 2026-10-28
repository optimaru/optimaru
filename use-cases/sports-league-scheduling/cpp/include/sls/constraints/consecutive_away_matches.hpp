#ifndef SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_HPP
#define SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard (default, weight 1): consecutiveAwayMatches — the away-side mirror of
// consecutiveHomeMatches, and the one that actually keeps teams from living
// on the road for a week.
struct ConsecutiveAwayMatches final : TeamSideRunConstraint
{
    explicit ConsecutiveAwayMatches(double weight = 1.0)
        : TeamSideRunConstraint("consecutiveAwayMatches", weight, /*hard=*/true)
    {
    }
    int teamOf(const Match &match) const override { return match.awayTeam; }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_CONSECUTIVE_AWAY_MATCHES_HPP
