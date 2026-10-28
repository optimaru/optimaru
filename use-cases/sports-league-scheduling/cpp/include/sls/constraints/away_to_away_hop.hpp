#ifndef SLS_CONSTRAINTS_AWAY_TO_AWAY_HOP_HPP
#define SLS_CONSTRAINTS_AWAY_TO_AWAY_HOP_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1): awayToAwayHop — a team visiting on two
// consecutive round indices travels straight from one host to the next. The
// pair is (m1 on index i, m2 on index i + 1) with m2.away == m1.away, costing
// dist(m1.home, m2.home).
struct AwayToAwayHop final : RoundIndexPairConstraint
{
    explicit AwayToAwayHop(double weight = 1.0)
        : RoundIndexPairConstraint("awayToAwayHop", weight, /*hard=*/false)
    {
    }

protected:
    std::int64_t marginal(const LeagueSchedule &schedule,
                          int matchIndex,
                          int index) const override
    {
        const Match &match = schedule.matches[matchIndex];
        std::int64_t kilometres = 0;
        for (int later : at(index + 1))
        {
            const Match &next = schedule.matches[later];
            if (next.awayTeam == match.awayTeam)
                kilometres += schedule.distance(match.homeTeam, next.homeTeam);
        }
        for (int earlier : at(index - 1))
        {
            const Match &previous = schedule.matches[earlier];
            if (match.awayTeam == previous.awayTeam)
                kilometres
                    += schedule.distance(previous.homeTeam, match.homeTeam);
        }
        return kilometres;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_AWAY_TO_AWAY_HOP_HPP
