#ifndef ES_CONSTRAINTS_UNASSIGNED_SHIFTS_COST_HPP
#define ES_CONSTRAINTS_UNASSIGNED_SHIFTS_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Hard, weight 1, global: unassignedShiftsCost (coverage guarantee). Raw
// cost = the number of shifts with no employee assigned, so 0 = every shift
// is staffed. Global because unassigned shifts belong to no unit: it tracks
// the assigned total from the members it is notified about.
struct UnassignedShiftsCost final : EsGlobalConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit UnassignedShiftsCost(double weight = kDefaultWeight)
        : EsGlobalConstraint("unassignedShiftsCost", weight, kDefaultLevel) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& members_per_unit) override {
        shiftCount_ = static_cast<long>(s.shifts.size());
        counts_.assign(members_per_unit.size(), 0);
        assigned_ = 0;
        for (std::size_t u = 0; u < members_per_unit.size(); ++u) {
            counts_[u] = static_cast<long>(members_per_unit[u].size());
            assigned_ += counts_[u];
        }
    }
    void on_unit_changed(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        const long n = static_cast<long>(members.size());
        assigned_ += n - counts_[unit];
        counts_[unit] = n;
    }
    bool supports_member_change() const override { return true; }
    void on_member_changed(const EmployeeSchedule& s, int unit,
                         const std::vector<int>& members, int, bool) override {
        on_unit_changed(s, unit, members);
    }
    double cost() const override { return static_cast<double>(shiftCount_ - assigned_); }

private:
    std::vector<long> counts_;
    long shiftCount_ = 0;
    long assigned_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNASSIGNED_SHIFTS_COST_HPP
