#ifndef FL_CONSTRAINTS_FACILITY_CAPACITY_HPP
#define FL_CONSTRAINTS_FACILITY_CAPACITY_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Hard (default): facilityCapacity — the demand a facility carries beyond its
// capacity. Java groups consumers by facility before summing their demand, so
// a facility nobody uses is never penalized; here that gate is `members
// .empty()`.
//
// No delta contract: max(0, demand - capacity) is not a sum of per-member
// terms (the clamp is what makes it non-additive), so the marginal cost of one
// consumer depends on the whole unit. cost() is O(members) either way, so the
// calculator's full re-cost path loses nothing.
struct FacilityCapacity final : FlUnitConstraint
{
    explicit FacilityCapacity(double weight = 1.0)
        : FlUnitConstraint("facility capacity", weight, /*hard=*/true)
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

#endif  // FL_CONSTRAINTS_FACILITY_CAPACITY_HPP
