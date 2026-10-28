#ifndef TS_CONSTRAINTS_ONE_ASSIGNMENT_PER_DATE_PER_TEAM_HPP
#define TS_CONSTRAINTS_ONE_ASSIGNMENT_PER_DATE_PER_TEAM_HPP

#include "ts/constraints/common.hpp"

namespace ts
{

// Hard (default, weight 1): oneAssignmentPerDatePerTeam — one point per pair
// of assignments a team holds on the same day (C(c, 2) per (team, day) cell).
// A sum over the pairs INSIDE one unit, so the exact delta of one assignment
// joining or leaving is the number of other members sharing its day.
struct OneAssignmentPerDatePerTeam final : TsUnitConstraint
{
    explicit OneAssignmentPerDatePerTeam(double weight = 1.0)
        : TsUnitConstraint("oneAssignmentPerDatePerTeam", weight, /*hard=*/true)
    {
    }
    void onReset(const TournamentSchedule &schedule) override
    {
        perDay_.assign(schedule.dayCount(), 0);
    }
    double cost(const TournamentSchedule &schedule,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        // Counting into a scratch histogram makes this O(members) instead of
        // O(members^2); the scratch is left zeroed for the next call.
        std::int64_t pairs = 0;
        for (int assignment : members)
            pairs += perDay_[detail::dayOf(schedule, assignment)]++;
        for (int assignment : members)
            perDay_[detail::dayOf(schedule, assignment)] = 0;
        return static_cast<double>(pairs);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const TournamentSchedule &schedule,
                     int /*unit*/,
                     const std::vector<int> &members,
                     int entity) const override
    {
        const int day = detail::dayOf(schedule, entity);
        std::int64_t sameDay = 0;
        for (int assignment : members)
        {
            if (assignment != entity
                && detail::dayOf(schedule, assignment) == day)
                ++sameDay;
        }
        return static_cast<double>(sameDay);
    }

private:
    // Scratch histogram, sized once per reset (mutable because cost() is
    // const by contract; the calculator drives one solve per instance).
    mutable std::vector<int> perDay_;
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_ONE_ASSIGNMENT_PER_DATE_PER_TEAM_HPP
