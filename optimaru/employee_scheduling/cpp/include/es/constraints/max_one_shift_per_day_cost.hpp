#ifndef ES_CONSTRAINTS_MAX_ONE_SHIFT_PER_DAY_COST_HPP
#define ES_CONSTRAINTS_MAX_ONE_SHIFT_PER_DAY_COST_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Hard, weight 1: maxOneShiftPerDayCost. Raw cost per employee = the number
// of PAIRS of its shifts that start on the same day (three shifts on one day
// cost 3), so 0 = at most one shift per day.
struct MaxOneShiftPerDayCost final : GroupableAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxOneShiftPerDayCost(double weight = kDefaultWeight)
        : GroupableAssignmentConstraint("maxOneShiftPerDayCost", weight, kDefaultLevel) {}
    void on_reset(const EmployeeSchedule& s) override {
        day_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
        }
    }
    double cost(const EmployeeSchedule&, int /*unit*/,
                const std::vector<int>& members) const override {
        double c = 0;
        for (std::size_t a = 0; a < members.size(); ++a) {
            for (std::size_t b = a + 1; b < members.size(); ++b) {
                if (day_[members[a]] == day_[members[b]]) c += 1;
            }
        }
        return c;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const EmployeeSchedule&, int /*unit*/,
                     const std::vector<int>& members, int entity) const override {
        const std::int32_t day = day_[entity];
        double c = 0;
        for (int i : members) {
            if (i != entity && day_[i] == day) c += 1;
        }
        return c;
    }

private:
    std::vector<std::int32_t> day_;  // dense copy of the shift start days
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_ONE_SHIFT_PER_DAY_COST_HPP
