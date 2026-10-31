#ifndef SLS_CONSTRAINTS_START_TO_AWAY_HOP_COST_HPP
#define SLS_CONSTRAINTS_START_TO_AWAY_HOP_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft, weight 1: startToAwayHopCost — the kilometres the away team travels to
// a match played on a round index with no round index right before it (the
// season's first round, or the first round after a gap in the indices),
// coming from its own home. Raw cost per round position = the summed
// dist(away, home) of its matches when round index - 1 does not exist, else
// 0.
struct StartToAwayHopCost final : PerMatchConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit StartToAwayHopCost(double weight = kDefaultWeight)
        : PerMatchConstraint("startToAwayHopCost", weight, kDefaultLevel)
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

#endif  // SLS_CONSTRAINTS_START_TO_AWAY_HOP_COST_HPP
