#ifndef SLS_CONSTRAINTS_AWAY_TO_END_HOP_HPP
#define SLS_CONSTRAINTS_AWAY_TO_END_HOP_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1): awayToEndHop — the kilometres the away team
// travels home after a match played on the LAST round index of the season (no
// round index follows it).
struct AwayToEndHop final : PerMatchConstraint
{
    explicit AwayToEndHop(double weight = 1.0)
        : PerMatchConstraint("awayToEndHop", weight, /*hard=*/false)
    {
    }
    double matchCost(const LeagueSchedule &schedule,
                     int unit,
                     int matchIndex) const override
    {
        const int index = schedule.roundIndexOf(unit);
        if (index != schedule.maxRoundIndex
            && schedule.roundIndexExists[index + 1])
            return 0.0;
        const Match &match = schedule.matches[matchIndex];
        return static_cast<double>(
            schedule.distance(match.homeTeam, match.awayTeam));
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_AWAY_TO_END_HOP_HPP
