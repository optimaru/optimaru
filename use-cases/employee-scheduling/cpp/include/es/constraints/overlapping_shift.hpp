#ifndef ES_CONSTRAINTS_OVERLAPPING_SHIFT_HPP
#define ES_CONSTRAINTS_OVERLAPPING_SHIFT_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Hard (default): overlap minutes between two shifts of the same employee.
struct OverlappingShift final : GroupableAssignmentConstraint {
    explicit OverlappingShift(double weight = 1.0)
        : GroupableAssignmentConstraint("Overlapping shift", weight, /*hard=*/true) {}
    void onReset(const EmployeeSchedule& s) override {
        start_.resize(s.shifts.size());
        end_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            start_[i] = static_cast<std::int32_t>(s.shifts[i].start);
            end_[i] = static_cast<std::int32_t>(s.shifts[i].end);
        }
    }
    double cost(const EmployeeSchedule&, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t c = 0;
        for (std::size_t a = 0; a < members.size(); ++a) {
            for (std::size_t b = a + 1; b < members.size(); ++b) {
                c += pairOverlap(members[a], members[b]);
            }
        }
        return static_cast<double>(c);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule&, int /*unit*/,
                     const std::vector<int>& members, int entity) const override {
        std::int64_t c = 0;
        for (int i : members) {
            if (i != entity) c += pairOverlap(entity, i);
        }
        return static_cast<double>(c);
    }
private:
    std::int32_t pairOverlap(int x, int y) const {
        const std::int32_t overlap = std::min(end_[x], end_[y]) - std::max(start_[x], start_[y]);
        return overlap > 0 ? overlap : 0;
    }
    std::vector<std::int32_t> start_, end_;  // dense copies of the shift times
};

}  // namespace es

#endif  // ES_CONSTRAINTS_OVERLAPPING_SHIFT_HPP
