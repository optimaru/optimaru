#ifndef ES_CONSTRAINTS_DESIRED_SHIFT_HPP
#define ES_CONSTRAINTS_DESIRED_SHIFT_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Soft (default), reward (negative cost): location-specific work preference.
// Unlike DesiredDay (rewards any work on a desired date), this rewards only when
// the worker is assigned a shift whose (start day, location) is in the worker's
// `desiredShifts` set — i.e. the requested LOCATION must match, not just the day.
// (Requesting one location but being given another earns nothing.) 1 per
// satisfied preference.
struct DesiredShift final : GroupableAssignmentConstraint {
    explicit DesiredShift(double weight = 1.0)
        : GroupableAssignmentConstraint("Desired shift for employee", weight, /*hard=*/false) {}
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

#endif  // ES_CONSTRAINTS_DESIRED_SHIFT_HPP
