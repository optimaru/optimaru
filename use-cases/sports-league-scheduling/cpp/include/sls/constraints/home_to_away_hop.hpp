#ifndef SLS_CONSTRAINTS_HOME_TO_AWAY_HOP_HPP
#define SLS_CONSTRAINTS_HOME_TO_AWAY_HOP_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1): homeToAwayHop — a team hosting on one round index
// and visiting on the next travels from its own home to the next host's home.
// The pair is (m1 on index i, m2 on index i + 1) with m2.away == m1.home,
// costing dist(m1.home, m2.home).
struct HomeToAwayHop final : RoundIndexPairConstraint
{
    explicit HomeToAwayHop(double weight = 1.0)
        : RoundIndexPairConstraint("homeToAwayHop", weight, /*hard=*/false)
    {
    }

protected:
    std::int64_t marginal(const LeagueSchedule &schedule,
                          int matchIndex,
                          int index) const override
    {
        const Match &match = schedule.matches[matchIndex];
        std::int64_t kilometres = 0;
        // As the earlier match of the pair.
        for (int later : at(index + 1))
        {
            const Match &next = schedule.matches[later];
            if (next.awayTeam == match.homeTeam)
                kilometres += schedule.distance(match.homeTeam, next.homeTeam);
        }
        // As the later match of the pair.
        for (int earlier : at(index - 1))
        {
            const Match &previous = schedule.matches[earlier];
            if (match.awayTeam == previous.homeTeam)
                kilometres
                    += schedule.distance(previous.homeTeam, match.homeTeam);
        }
        return kilometres;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_HOME_TO_AWAY_HOP_HPP
