#ifndef ES_CONSTRAINTS_UNASSIGNED_SHIFT_HPP
#define ES_CONSTRAINTS_UNASSIGNED_SHIFT_HPP

#include "es/constraints/common.hpp"

namespace es {

// Hard (default): 1 per shift with no employee assigned (coverage guarantee).
// Global because unassigned shifts belong to no unit: it tracks the assigned
// total from the members it is notified about.
struct UnassignedShift final : EsGlobalConstraint {
    explicit UnassignedShift(double weight = 1.0)
        : EsGlobalConstraint("Unassigned shift", weight, /*hard=*/true) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        shiftCount_ = static_cast<long>(s.shifts.size());
        counts_.assign(membersPerUnit.size(), 0);
        assigned_ = 0;
        for (std::size_t u = 0; u < membersPerUnit.size(); ++u) {
            counts_[u] = static_cast<long>(membersPerUnit[u].size());
            assigned_ += counts_[u];
        }
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        const long n = static_cast<long>(members.size());
        assigned_ += n - counts_[unit];
        counts_[unit] = n;
    }
    bool supportsMemberChange() const override { return true; }
    void onMemberChanged(const EmployeeSchedule& s, int unit,
                         const std::vector<int>& members, int, bool) override {
        onUnitChanged(s, unit, members);
    }
    double cost() const override { return static_cast<double>(shiftCount_ - assigned_); }

private:
    std::vector<long> counts_;
    long shiftCount_ = 0;
    long assigned_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNASSIGNED_SHIFT_HPP
