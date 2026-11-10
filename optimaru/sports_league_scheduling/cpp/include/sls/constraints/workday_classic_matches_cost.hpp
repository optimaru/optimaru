#ifndef SLS_CONSTRAINTS_WORKDAY_CLASSIC_MATCHES_COST_HPP
#define SLS_CONSTRAINTS_WORKDAY_CLASSIC_MATCHES_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls {

// Soft, weight 1'000: workdayClassicMatchesCost — a classic (derby) match
// played on a workday, i.e. a round that is neither a weekend nor a holiday.
// Raw cost per round position = the number of classic matches in it when the
// round is not a weekend/holiday, else 0; 0 = every derby is on a weekend or
// holiday. The default weight is expensive enough against the
// kilometre-denominated travel terms that the search will detour a long way
// to keep a derby on a Saturday.
struct WorkdayClassicMatchesCost final : PerMatchConstraint {
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WorkdayClassicMatchesCost(double weight = kDefaultWeight)
        : PerMatchConstraint("workdayClassicMatchesCost", weight, kDefaultLevel) {}
    double matchCost(const LeagueSchedule& schedule, int unit, int matchIndex) const override {
        return schedule.matches[matchIndex].classicMatch && !schedule.rounds[unit].weekendOrHoliday
                       ? 1.0
                       : 0.0;
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_WORKDAY_CLASSIC_MATCHES_COST_HPP
