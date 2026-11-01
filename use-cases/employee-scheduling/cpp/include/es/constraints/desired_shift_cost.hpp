#ifndef ES_CONSTRAINTS_DESIRED_SHIFT_COST_HPP
#define ES_CONSTRAINTS_DESIRED_SHIFT_COST_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Soft, weight 1, REWARD: desiredShiftCost, a location-specific work
// preference. Unlike DesiredDayCost (any work on a desired date), it counts
// only a shift whose (start day, location) is in the worker's `desiredShifts`
// set — the requested LOCATION must match, not just the day (being given
// another location earns nothing). Raw cost per employee = minus 1 per
// satisfied preference: raw <= 0, and 0 = no wish is granted. A reward can
// never be hard (reward = true, levelFlippable = false).
struct DesiredShiftCost final : GroupableAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DesiredShiftCost(double weight = kDefaultWeight)
        : GroupableAssignmentConstraint("desiredShiftCost", weight, kDefaultLevel) {
        reward = true;
        levelFlippable = false;
    }
    void onReset(const EmployeeSchedule& s) override {
        shiftCount_ = static_cast<int>(s.shifts.size());
        match_.assign(s.employees.size() * s.shifts.size(), 0);
        for (std::size_t e = 0; e < s.employees.size(); ++e) {
            const auto& want = s.employees[e].desiredShifts;
            if (want.empty()) continue;
            for (std::size_t i = 0; i < s.shifts.size(); ++i) {
                match_[e * s.shifts.size() + i] =
                        want.count({s.shifts[i].startDay, s.shifts[i].location}) ? 1 : 0;
            }
        }
    }
    double cost(const EmployeeSchedule&, int unit,
                const std::vector<int>& members) const override {
        const std::uint8_t* row = match_.data() + static_cast<std::size_t>(unit) * shiftCount_;
        double c = 0;
        for (int i : members) c += row[i];
        return -c;  // reward
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule&, int unit, const std::vector<int>&,
                     int entity) const override {
        return memberDelta(unit, entity);
    }
    double memberDelta(int unit, int entity) const {
        return -static_cast<double>(
                match_[static_cast<std::size_t>(unit) * shiftCount_ + entity]);
    }

private:
    std::vector<std::uint8_t> match_;  // (employee, shift) -> satisfies a desired (day,loc)
    int shiftCount_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DESIRED_SHIFT_COST_HPP
