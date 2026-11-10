#ifndef SLS_CONSTRAINTS_AWAY_TO_END_HOP_COST_HPP
#define SLS_CONSTRAINTS_AWAY_TO_END_HOP_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft, weight 1: awayToEndHopCost — the kilometres the away team travels
// home after a match played on a round index with no round index right after
// it (the season's last round, or the last round before a gap in the
// indices). Raw cost per round position = the summed dist(home, away) of its
// matches when round index + 1 does not exist, else 0.
struct AwayToEndHopCost final : PerMatchConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AwayToEndHopCost(double weight = kDefaultWeight)
        : PerMatchConstraint("awayToEndHopCost", weight, kDefaultLevel)
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

#endif  // SLS_CONSTRAINTS_AWAY_TO_END_HOP_COST_HPP
