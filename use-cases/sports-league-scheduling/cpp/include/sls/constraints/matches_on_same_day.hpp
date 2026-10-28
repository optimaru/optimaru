#ifndef SLS_CONSTRAINTS_MATCHES_ON_SAME_DAY_HPP
#define SLS_CONSTRAINTS_MATCHES_ON_SAME_DAY_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard (default, weight 1): matchesOnSameDay — one point per pair of matches
// on the same round index that share at least one team. A pair sharing BOTH
// teams (a fixture and its reverse) still counts once, which is why the
// marginal counts matches rather than team occurrences.
struct MatchesOnSameDay final : RoundIndexPairConstraint
{
    explicit MatchesOnSameDay(double weight = 1.0)
        : RoundIndexPairConstraint("matchesOnSameDay", weight, /*hard=*/true)
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

#endif  // SLS_CONSTRAINTS_MATCHES_ON_SAME_DAY_HPP
