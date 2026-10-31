#ifndef ES_CONSTRAINTS_ASSIGNMENT_COST_HPP
#define ES_CONSTRAINTS_ASSIGNMENT_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft (default): monetary cost of the assignment. Per
// (employee, shift): an explicit matrix entry if present, otherwise
// costPerHour * duration, otherwise costPerShift. Fully incremental (constant-time
// delta) via a precomputed table.
struct AssignmentCost final : EsUnitConstraint {
    using CostMatrix = std::map<std::string, std::map<std::string, double>>;
    explicit AssignmentCost(double weight = 1.0, double costPerHour = 0.0,
                            double costPerShift = 0.0, CostMatrix matrix = {})
        : EsUnitConstraint("Assignment cost", weight, /*hard=*/false),
          costPerHour_(costPerHour), costPerShift_(costPerShift),
          matrix_(std::move(matrix)) {}
    void onReset(const EmployeeSchedule& s) override {
        shiftCount_ = static_cast<int>(s.shifts.size());
        table_.assign(s.employees.size() * s.shifts.size(), 0.0);
        for (std::size_t e = 0; e < s.employees.size(); ++e) {
            const auto row = matrix_.find(s.employees[e].name);
            const std::map<std::string, double>* overrides =
                    row != matrix_.end() ? &row->second : nullptr;
            for (std::size_t i = 0; i < s.shifts.size(); ++i) {
                const Shift& shift = s.shifts[i];
                double c = costPerShift_;
                if (costPerHour_ > 0.0) {
                    c = costPerHour_ * static_cast<double>(shift.end - shift.start) / 60.0;
                }
                if (overrides != nullptr) {
                    const auto entry = overrides->find(shift.id);
                    if (entry != overrides->end()) c = entry->second;
                }
                table_[e * s.shifts.size() + i] = c;
            }
        }
    }
    double cost(const EmployeeSchedule&, int unit,
                const std::vector<int>& members) const override {
        const double* row = table_.data() + static_cast<std::size_t>(unit) * shiftCount_;
        double c = 0;
        for (int i : members) c += row[i];
        return c;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule&, int unit, const std::vector<int>&,
                     int entity) const override {
        return table_[static_cast<std::size_t>(unit) * shiftCount_ + entity];
    }

private:
    double costPerHour_, costPerShift_;
    CostMatrix matrix_;
    std::vector<double> table_;  // (employee, shift) -> money
    int shiftCount_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_ASSIGNMENT_COST_HPP
