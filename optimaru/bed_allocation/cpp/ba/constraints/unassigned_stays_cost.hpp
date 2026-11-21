#ifndef BA_CONSTRAINTS_UNASSIGNED_STAYS_COST_HPP
#define BA_CONSTRAINTS_UNASSIGNED_STAYS_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba {

// Medium, weight 1: unassignedStaysCost — every night a stay spends in the
// "no bed" sentinel (the stay is left unassigned). Raw cost (on the sentinel
// unit only) = the summed nights of the stays left without a bed, so 0 =
// every stay has a bed. An uninitialized stay (bed == -1) is not on the
// sentinel and costs nothing.
//
// The weight is in MEDIUM units: the domain folds the medium level into the
// engine's soft level as `medium * kMediumUnit` (kMediumFold in domain.hpp),
// and the decomposed calculator applies that fold, so the component carries
// no 10^9 factor. hard() makes it 1 hard per night, soft() 1 soft per night
// (outside the fold), and an explicit weight is in units of the level in
// effect (medium by default).
//
// The only unit this can ever be non-zero for is the sentinel, so it declares
// that scope and the calculator skips it on every real bed.
struct UnassignedStaysCost final : PerStayConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit UnassignedStaysCost(double weight = kDefaultWeight)
        : PerStayConstraint("unassignedStaysCost", weight, kDefaultLevel) {}
    void on_reset(const BedPlan& plan) override {
        PerStayConstraint::on_reset(plan);
        sentinel_.assign(1, plan.noBedValue());
    }
    const std::vector<int>* scoped_units() const override { return &sentinel_; }
    double stayCost(const BedPlan& plan, int unit, int stayIndex) const override {
        if (unit != plan.noBedValue()) return 0.0;
        return static_cast<double>(plan.stays[stayIndex].nightCount());
    }

private:
    std::vector<int> sentinel_;
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_UNASSIGNED_STAYS_COST_HPP
