#ifndef SLS_CONSTRAINTS_CLASSIC_MATCHES_HPP
#define SLS_CONSTRAINTS_CLASSIC_MATCHES_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Soft (default, weight 1'000): classicMatches — a classic (derby) match
// played on a round that is neither a weekend nor a holiday. Expensive enough
// against the kilometre-denominated travel terms that the search will detour
// a long way to keep a derby on a Saturday.
struct ClassicMatches final : PerMatchConstraint
{
    explicit ClassicMatches(double weight = 1000.0)
        : PerMatchConstraint("classicMatches", weight, /*hard=*/false)
    {
    }
    double matchCost(const LeagueSchedule &schedule,
                     int unit,
                     int matchIndex) const override
    {
        return schedule.matches[matchIndex].classicMatch
                       && !schedule.rounds[unit].weekendOrHoliday
                   ? 1.0
                   : 0.0;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_CLASSIC_MATCHES_HPP
