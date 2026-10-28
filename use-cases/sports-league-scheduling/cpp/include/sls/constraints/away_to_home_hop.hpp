#ifndef SLS_CONSTRAINTS_AWAY_TO_HOME_HOP_HPP
#define SLS_CONSTRAINTS_AWAY_TO_HOME_HOP_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1): awayToHomeHop — a team visiting on one round
// index and hosting on the next travels back home. The pair is (m1 on index
// i, m2 on index i + 1) with m2.home == m1.away, and the distance is the
// earlier match's own dist(m1.home, m1.away) — it depends only on m1, so as
// the LATER match a candidate contributes the earlier one's distance instead
// of its own.
struct AwayToHomeHop final : RoundIndexPairConstraint
{
    explicit AwayToHomeHop(double weight = 1.0)
        : RoundIndexPairConstraint("awayToHomeHop", weight, /*hard=*/false)
    {
    }

protected:
    std::int64_t marginal(const LeagueSchedule &schedule,
                          int matchIndex,
                          int index) const override
    {
        const Match &match = schedule.matches[matchIndex];
        std::int64_t kilometres = 0;
        const std::int64_t own
            = schedule.distance(match.homeTeam, match.awayTeam);
        for (int later : at(index + 1))
        {
            if (schedule.matches[later].homeTeam == match.awayTeam)
                kilometres += own;
        }
        for (int earlier : at(index - 1))
        {
            const Match &previous = schedule.matches[earlier];
            if (match.homeTeam == previous.awayTeam)
                kilometres
                    += schedule.distance(previous.homeTeam, previous.awayTeam);
        }
        return kilometres;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_AWAY_TO_HOME_HOP_HPP
