#ifndef FL_CONSTRAINTS_FACILITY_CAPACITY_COST_HPP
#define FL_CONSTRAINTS_FACILITY_CAPACITY_COST_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Hard, weight 1: facilityCapacityCost — the demand a facility carries beyond
// its capacity. Raw cost per facility = max(0, assigned demand - capacity),
// so 0 = within capacity. A facility nobody uses is never penalized; that
// gate is `members.empty()`.
//
// No delta contract: max(0, demand - capacity) is not a sum of per-member
// terms (the clamp is what makes it non-additive), so the marginal cost of one
// consumer depends on the whole unit. cost() is O(members) either way, so the
// calculator's full re-cost path loses nothing.
struct FacilityCapacityCost final : FlUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FacilityCapacityCost(double weight = kDefaultWeight)
        : FlUnitConstraint("facilityCapacityCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const FacilityLocationProblem &problem) override
    {
        detail::requirePrepared(problem);
    }
    double cost(const FacilityLocationProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        if (members.empty())
            return 0.0;
        std::int64_t demand = 0;
        for (int consumer : members)
            demand += problem.consumers[consumer].demand;
        const std::int64_t over = demand - problem.facilities[unit].capacity;
        return over > 0 ? static_cast<double>(over) : 0.0;
    }
};

}  // namespace fl

#endif  // FL_CONSTRAINTS_FACILITY_CAPACITY_COST_HPP
