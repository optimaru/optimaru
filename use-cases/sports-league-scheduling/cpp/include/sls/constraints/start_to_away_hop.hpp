#ifndef SLS_CONSTRAINTS_START_TO_AWAY_HOP_HPP
#define SLS_CONSTRAINTS_START_TO_AWAY_HOP_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1): startToAwayHop — the kilometres the away team
// travels to a match played on the FIRST round index of the season (no round
// index precedes it), coming from its own home.
struct StartToAwayHop final : PerMatchConstraint
{
    explicit StartToAwayHop(double weight = 1.0)
        : PerMatchConstraint("startToAwayHop", weight, /*hard=*/false)
    {
    }
    double matchCost(const LeagueSchedule &schedule,
                     int unit,
                     int matchIndex) const override
    {
        const int index = schedule.roundIndexOf(unit);
        if (index != 0 && schedule.roundIndexExists[index - 1])
            return 0.0;
        const Match &match = schedule.matches[matchIndex];
        return static_cast<double>(
            schedule.distance(match.awayTeam, match.homeTeam));
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_START_TO_AWAY_HOP_HPP
