#ifndef QAP_CONSTRAINTS_LOCATION_CAPACITY_COST_HPP
#define QAP_CONSTRAINTS_LOCATION_CAPACITY_COST_HPP

#include "qap/constraints/common.hpp"

namespace qap
{

// Hard, weight 1: locationCapacityCost — demand standing at a location beyond
// its capacity. Raw cost per location = max(0, summed demand of its facilities
// - capacity) for a location with a capacity, 0 for one without (no limit);
// a facility missing from facility_demands has demand 0. 0 = every location is
// within its capacity. Same per-location value as the 0.1.x Python scorer, in
// location order.
//
// No delta contract: the clamp makes it non-additive. cost() is O(members),
// O(1) in a bijection, so the full re-cost path loses nothing.
struct LocationCapacityCost final : QapUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit LocationCapacityCost(double weight = kDefaultWeight)
        : QapUnitConstraint("locationCapacityCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const QuadraticAssignmentProblem &problem) override
    {
        detail::requirePrepared(problem);
        scope_ = &problem.capacitatedLocations;
    }
    double cost(const QuadraticAssignmentProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        if (!problem.hasCapacity[unit])
            return 0.0;
        std::int64_t demand = 0;
        for (int facility : members)
            demand += problem.demandOf[facility];
        const std::int64_t over = demand - problem.capacityOf[unit];
        return over > 0 ? static_cast<double>(over) : 0.0;
    }
    const std::vector<int> *scoped_units() const override { return scope_; }

private:
    const std::vector<int> *scope_ = nullptr;
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_LOCATION_CAPACITY_COST_HPP
