#ifndef BA_CONSTRAINTS_ASSIGN_EVERY_PATIENT_TO_A_BED_HPP
#define BA_CONSTRAINTS_ASSIGN_EVERY_PATIENT_TO_A_BED_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Medium (default, weight 1 medium level): assignEveryPatientToABed — every
// night a stay spends in the "no bed" sentinel (the quickstart's null bed).
//
// The medium level is folded into the engine's soft level as
// `medium * kMediumUnit` (see domain.hpp), so the default weight carries that
// factor: the component's own cost stays plain nights, which keeps it
// readable and lets a caller re-price it like any other soft term.
//
// The only unit this can ever be non-zero for is the sentinel, so it declares
// that scope and the calculator skips it on every real bed.
struct AssignEveryPatientToABed final : PerStayConstraint
{
    explicit AssignEveryPatientToABed(double weight
                                      = static_cast<double>(kMediumUnit))
        : PerStayConstraint("assignEveryPatientToABed", weight, /*hard=*/false)
    {
    }
    void onReset(const BedPlan &plan) override
    {
        PerStayConstraint::onReset(plan);
        sentinel_.assign(1, plan.noBedValue());
    }
    const std::vector<int> *scopedUnits() const override { return &sentinel_; }
    double stayCost(const BedPlan &plan, int unit, int stayIndex) const override
    {
        if (unit != plan.noBedValue())
            return 0.0;
        return static_cast<double>(plan.stays[stayIndex].nightCount());
    }

private:
    std::vector<int> sentinel_;
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_ASSIGN_EVERY_PATIENT_TO_A_BED_HPP
