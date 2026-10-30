#ifndef ES_CONSTRAINTS_DATE_OVERLAP_CONSTRAINT_HPP
#define ES_CONSTRAINTS_DATE_OVERLAP_CONSTRAINT_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Shared base for the three date-overlap costs (unavailable / undesired /
// desired): a per-(employee, shift) minute table selected by an Employee date
// set, added with a sign (+1 penalty, -1 reward).
struct DateOverlapConstraint : GroupableAssignmentConstraint {
    DateOverlapConstraint(std::string name, double weight, bool hard,
                          std::set<DayIndex> Employee::* dates, double sign,
                          bool perDay = false)
        : GroupableAssignmentConstraint(std::move(name), weight, hard), dates_(dates), sign_(sign),
          perDay_(perDay) {}
    void onReset(const EmployeeSchedule& s) override {
        shiftCount_ = static_cast<int>(s.shifts.size());
        minutes_.assign(s.employees.size() * s.shifts.size(), 0);
        for (std::size_t e = 0; e < s.employees.size(); ++e) {
            const std::set<DayIndex>& dates = s.employees[e].*dates_;
            if (dates.empty()) continue;
            for (std::size_t i = 0; i < s.shifts.size(); ++i) {
                const std::int32_t ov = static_cast<std::int32_t>(
                        ScoreCalculator::overlapWithDates(s.shifts[i], dates));
                // perDay: count a matching shift as 1 (unit-normalized) instead
                // of its overlap minutes, so preference cost is comparable to the
                // count-based soft rules.
                minutes_[e * s.shifts.size() + i] = perDay_ ? (ov > 0 ? 1 : 0) : ov;
            }
        }
    }
    double cost(const EmployeeSchedule& s, int unit,
                const std::vector<int>& members) const override {
        const std::int32_t* row =
                minutes_.data() + static_cast<std::size_t>(unit) * shiftCount_;
        std::int64_t c = 0;
        for (int i : members)
            if (!s.shifts[i].pinned) c += row[i];  // pinned shifts are fixed context
        return sign_ * static_cast<double>(c);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule& s, int unit, const std::vector<int>&,
                     int entity) const override {
        return memberDelta(s, unit, entity);
    }
    double memberDelta(const EmployeeSchedule& s, int unit, int entity) const {
        if (s.shifts[entity].pinned) return 0.0;
        return sign_ * minutes_[static_cast<std::size_t>(unit) * shiftCount_ + entity];
    }
private:
    std::set<DayIndex> Employee::* dates_;
    double sign_;
    bool perDay_;
    std::vector<std::int32_t> minutes_;  // (employee, shift) -> overlap minutes, or 0/1 if perDay
    int shiftCount_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DATE_OVERLAP_CONSTRAINT_HPP
