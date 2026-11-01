#ifndef SLS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP
#define SLS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard, weight 1: teamConflictCost — a team double-booked on one round index
// (day). Raw cost (one global total) = the number of unordered pairs of
// matches on the same round index that share at least one team; a pair
// sharing BOTH teams (a fixture and its reverse) still counts once, which is
// why the marginal counts matches rather than team occurrences. 0 = no team
// plays twice on one round index.
struct TeamConflictCost final : RoundIndexPairConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeamConflictCost(double weight = kDefaultWeight)
        : RoundIndexPairConstraint("teamConflictCost", weight, kDefaultLevel)
    {
    }

protected:
    std::int64_t marginal(const LeagueSchedule &schedule,
                          int matchIndex,
                          int index) const override
    {
        const Match &match = schedule.matches[matchIndex];
        std::int64_t shared = 0;
        for (int other : at(index))
        {
            const Match &opponent = schedule.matches[other];
            if (match.homeTeam == opponent.homeTeam
                || match.homeTeam == opponent.awayTeam
                || match.awayTeam == opponent.homeTeam
                || match.awayTeam == opponent.awayTeam)
            {
                ++shared;
            }
        }
        return shared;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP
