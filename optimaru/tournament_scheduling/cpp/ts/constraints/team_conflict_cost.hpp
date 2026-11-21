#ifndef TS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP
#define TS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP

#include "ts/constraints/common.hpp"

namespace ts {

// Hard, weight 1: teamConflictCost — a team double-booked on one day. Raw
// cost per team = the number of pairs of assignments it holds on the same day
// (C(c, 2) per (team, day) cell, summed over the team's days), so 0 = the team
// plays at most once per day.
// A sum over the pairs INSIDE one unit, so the exact delta of one assignment
// joining or leaving is the number of other members sharing its day.
struct TeamConflictCost final : TsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeamConflictCost(double weight = kDefaultWeight)
        : TsUnitConstraint("teamConflictCost", weight, kDefaultLevel) {}
    void on_reset(const TournamentSchedule& schedule) override {
        perDay_.assign(schedule.dayCount(), 0);
    }
    double cost(const TournamentSchedule& schedule, int /*unit*/,
                const std::vector<int>& members) const override {
        // Counting into a scratch histogram makes this O(members) instead of
        // O(members^2); the scratch is left zeroed for the next call.
        std::int64_t pairs = 0;
        for (int assignment : members) pairs += perDay_[detail::dayOf(schedule, assignment)]++;
        for (int assignment : members) perDay_[detail::dayOf(schedule, assignment)] = 0;
        return static_cast<double>(pairs);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const TournamentSchedule& schedule, int /*unit*/,
                      const std::vector<int>& members, int entity) const override {
        const int day = detail::dayOf(schedule, entity);
        std::int64_t sameDay = 0;
        for (int assignment : members) {
            if (assignment != entity && detail::dayOf(schedule, assignment) == day) ++sameDay;
        }
        return static_cast<double>(sameDay);
    }

private:
    // Scratch histogram, sized once per reset (mutable because cost() is
    // const by contract; the calculator drives one solve per instance).
    mutable std::vector<int> perDay_;
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_TEAM_CONFLICT_COST_HPP
