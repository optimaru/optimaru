#ifndef QAP_CONSTRAINTS_ASSIGNMENT_COST_HPP
#define QAP_CONSTRAINTS_ASSIGNMENT_COST_HPP

#include "qap/constraints/common.hpp"

namespace qap
{

// Soft, weight 1: assignmentCost — the direct facility-to-location cost of the
// assignment. Raw cost = sum over assigned facilities f of
// assignment_costs[f][location of f]; 0 without an assignment_costs matrix.
// One number for the whole assignment (a GlobalConstraint, so the exponent
// shapes the total, as the 0.1.x single-entry list did). The matrix is not
// validated >= 0, so the total may be negative (scored as a bonus).
//
// Member-change fast path: a facility joining a location adds its cost there,
// leaving subtracts it — O(1) per event.
struct AssignmentCost final : QapMemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AssignmentCost(double weight = kDefaultWeight)
        : QapMemberChangeGlobalConstraint("assignmentCost", weight, kDefaultLevel)
    {
    }
    void reset(const QuadraticAssignmentProblem &problem,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(problem);
        total_ = 0;
        for (std::size_t location = 0; location < membersPerUnit.size(); ++location)
        {
            for (int facility : membersPerUnit[location])
                total_ += problem.directCostOf(facility, static_cast<int>(location));
        }
    }
    void onMemberChanged(const QuadraticAssignmentProblem &problem,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        const std::int64_t cost = problem.directCostOf(entity, unit);
        total_ += joined ? cost : -cost;
    }
    double cost() const override { return static_cast<double>(total_); }

private:
    std::int64_t total_ = 0;
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_ASSIGNMENT_COST_HPP
