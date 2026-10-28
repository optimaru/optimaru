#ifndef FL_CONSTRAINTS_FACILITY_SETUP_COST_HPP
#define FL_CONSTRAINTS_FACILITY_SETUP_COST_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Soft (default, weight 2): facilitySetupCost — a facility serving at least
// one consumer costs its setup price once. A step function of "is this
// facility used at all", which is why the domain ships CloseFacilityMove: no
// single-consumer move can cross the step.
//
// No delta contract needed: cost() is already O(1) (it only looks at whether
// `members` is empty), so the full re-cost path is as cheap as a marginal one.
struct FacilitySetupCost final : FlUnitConstraint
{
    explicit FacilitySetupCost(double weight = 2.0)
        : FlUnitConstraint("facility setup cost", weight, /*hard=*/false)
    {
    }
    void onReset(const FacilityLocationProblem &problem) override
    {
        detail::requirePrepared(problem);
    }
    double cost(const FacilityLocationProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        return members.empty()
                   ? 0.0
                   : static_cast<double>(problem.facilities[unit].setupCost);
    }
};

}  // namespace fl

#endif  // FL_CONSTRAINTS_FACILITY_SETUP_COST_HPP
