#ifndef FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP
#define FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Soft, weight 6: distanceFromFacilityCost — the meters every assigned
// consumer stands from its facility. Raw cost per facility = the summed
// distance of its consumers (0 for an unused facility). The provider declares
// ofSoft(5), but the quickstart solution overrides it to 6 with
// ConstraintWeightOverrides, so 6 is the effective default carried here
// (before 0.2.0 it was also stored on the problem as
// distanceFromFacilityWeight).
//
// A pure sum of per-member terms, so the exact delta is one precomputed matrix
// lookup: the calculator maintains it incrementally in O(1) per move.
struct DistanceFromFacilityCost final : FlUnitConstraint
{
    static constexpr double kDefaultWeight = 6.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DistanceFromFacilityCost(double weight = kDefaultWeight)
        : FlUnitConstraint("distanceFromFacilityCost", weight, kDefaultLevel)
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
        std::int64_t meters = 0;
        for (int consumer : members)
            meters += problem.distanceOf(consumer, unit);
        return static_cast<double>(meters);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const FacilityLocationProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return static_cast<double>(problem.distanceOf(entity, unit));
    }
};

}  // namespace fl

#endif  // FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP
