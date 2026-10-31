#ifndef ES_CONSTRAINTS_REQUIRED_SKILL_HPP
#define ES_CONSTRAINTS_REQUIRED_SKILL_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Hard (default): 1 per assigned shift whose required skill the employee lacks.
struct RequiredSkill final : GroupableAssignmentConstraint {
    explicit RequiredSkill(double weight = 1.0)
        : GroupableAssignmentConstraint("Missing required skill", weight, /*hard=*/true) {}
    void onReset(const EmployeeSchedule& s) override {
        shiftCount_ = static_cast<int>(s.shifts.size());
        miss_.assign(s.employees.size() * s.shifts.size(), 0);
        for (std::size_t e = 0; e < s.employees.size(); ++e) {
            const std::uint64_t mask = s.employees[e].skillMask;
            for (std::size_t i = 0; i < s.shifts.size(); ++i) {
                miss_[e * s.shifts.size() + i] =
                        (s.shifts[i].requiredSkillBit != 0 &&
                         (mask & s.shifts[i].requiredSkillBit) == 0)
                                ? 1
                                : 0;  // bit 0 == no skill requirement
            }
        }
    }
    double cost(const EmployeeSchedule& s, int unit,
                const std::vector<int>& members) const override {
        const std::uint8_t* row = miss_.data() + static_cast<std::size_t>(unit) * shiftCount_;
        double c = 0;
        for (int i : members)
            if (!s.shifts[i].pinned) c += row[i];  // pinned = settled history, not re-judged
        return c;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule& s, int unit, const std::vector<int>&,
                     int entity) const override {
        return memberDelta(s, unit, entity);
    }
    double memberDelta(const EmployeeSchedule& s, int unit, int entity) const {
        if (s.shifts[entity].pinned) return 0.0;
        return miss_[static_cast<std::size_t>(unit) * shiftCount_ + entity];
    }

private:
    std::vector<std::uint8_t> miss_;  // (employee, shift) -> lacks the skill
    int shiftCount_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_REQUIRED_SKILL_HPP
