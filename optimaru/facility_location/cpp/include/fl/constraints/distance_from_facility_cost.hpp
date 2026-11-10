#ifndef FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP
#define FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Soft, weight 6: distanceFromFacilityCost — the meters every assigned
// consumer stands from its facility. Raw cost per facility = the summed
// distance of its consumers (0 for an unused facility).
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
    void on_reset(const FacilityLocationProblem &problem) override
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
    bool supports_delta() const override { return true; }
    double delta_cost(const FacilityLocationProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return static_cast<double>(problem.distanceOf(entity, unit));
    }
};

}  // namespace fl

#endif  // FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_COST_HPP
