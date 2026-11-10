#ifndef SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_COST_HPP
#define SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard, weight 1: consecutiveHomeMatchesCost — a team playing at home on four
// or more consecutive round indices. Raw cost (one global total) = summed over
// teams, the WHOLE length of every such run (a run of 4 costs 4, not the
// excess over 3). Several home matches on one
// index count once (the presence of the index is what forms the run). 0 = no
// team has a home run of 4 or more.
struct ConsecutiveHomeMatchesCost final : TeamSideRunConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ConsecutiveHomeMatchesCost(double weight = kDefaultWeight)
        : TeamSideRunConstraint(
              "consecutiveHomeMatchesCost", weight, kDefaultLevel)
    {
    }
    int teamOf(const Match &match) const override { return match.homeTeam; }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_CONSECUTIVE_HOME_MATCHES_COST_HPP
