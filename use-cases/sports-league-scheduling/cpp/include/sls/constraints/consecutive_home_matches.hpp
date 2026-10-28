#ifndef SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_HPP
#define SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard (default, weight 1): consecutiveHomeMatches — a team playing at home
// on four or more consecutive round indices is penalized by the whole run
// length. Several home matches on one index count once (the presence of the
// index is what forms the run).
struct ConsecutiveHomeMatches final : TeamSideRunConstraint
{
    explicit ConsecutiveHomeMatches(double weight = 1.0)
        : TeamSideRunConstraint("consecutiveHomeMatches", weight, /*hard=*/true)
    {
    }
    int teamOf(const Match &match) const override { return match.homeTeam; }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_HPP
