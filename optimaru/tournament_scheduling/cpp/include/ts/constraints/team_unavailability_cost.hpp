#ifndef TS_CONSTRAINTS_TEAM_UNAVAILABILITY_COST_HPP
#define TS_CONSTRAINTS_TEAM_UNAVAILABILITY_COST_HPP

#include "ts/constraints/common.hpp"

namespace ts
{

// Hard, weight 1: teamUnavailabilityCost — a team playing on a day it is
// unavailable. Raw cost per team = the number of its UnavailabilityPenalty
// FACTS whose (team, day) cell holds at least one assignment, so 0 = the team
// plays on none of its unavailable days. Per fact, not per assignment: two
// facts for the same cell cost two, one fact over a cell with three
// assignments still costs one (the Java
// forEach(UnavailabilityPenalty).ifExists(TeamAssignment)).
//
// That "at least one" is a clamp, so the cost is not a sum of per-member
// terms and the component offers no delta; the calculator re-costs a touched
// team in O(members + facts of that team). It only ever applies to teams that
// actually carry a fact, and declares exactly that scope, so the calculator
// skips it on every other team.
struct TeamUnavailabilityCost final : TsUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeamUnavailabilityCost(double weight = kDefaultWeight)
        : TsUnitConstraint("teamUnavailabilityCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const TournamentSchedule &schedule) override
    {
        // Per team: the days it is unavailable on, WITH duplicates — one
        // entry per fact, which is what the per-fact count means.
        daysByTeam_.assign(schedule.teamCount(), {});
        for (const UnavailabilityPenalty &penalty :
             schedule.unavailabilityPenalties)
        {
            if (penalty.team >= 0 && penalty.team < schedule.teamCount())
                daysByTeam_[penalty.team].push_back(penalty.day);
        }
        scoped_.clear();
        for (int team = 0; team < schedule.teamCount(); ++team)
        {
            if (!daysByTeam_[team].empty())
                scoped_.push_back(team);
        }
        occupied_.assign(schedule.dayCount(), 0);
    }
    const std::vector<int> *scoped_units() const override { return &scoped_; }
    double cost(const TournamentSchedule &schedule,
                int unit,
                const std::vector<int> &members) const override
    {
        const std::vector<int> &days = daysByTeam_[unit];
        if (days.empty())
            return 0.0;
        for (int assignment : members)
            occupied_[detail::dayOf(schedule, assignment)] = 1;
        std::int64_t hits = 0;
        for (int day : days)
            hits += occupied_[day];
        for (int assignment : members)
            occupied_[detail::dayOf(schedule, assignment)] = 0;
        return static_cast<double>(hits);
    }

private:
    std::vector<std::vector<int>> daysByTeam_;
    std::vector<int> scoped_;             // teams carrying at least one fact
    mutable std::vector<char> occupied_;  // scratch: day -> team busy there
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_TEAM_UNAVAILABILITY_COST_HPP
