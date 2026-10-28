#ifndef FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_HPP
#define FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_HPP

#include "fl/constraints/common.hpp"

namespace fl
{

// Soft (default, weight 6): distanceFromFacility — the meters every assigned
// consumer stands from its facility. The provider declares ofSoft(5), but the
// quickstart solution overrides it to 6 (see domain.hpp), which is the default
// carried here.
//
// A pure sum of per-member terms, so the exact delta is one precomputed matrix
// lookup: the calculator maintains it incrementally in O(1) per move.
struct DistanceFromFacility final : FlUnitConstraint
{
    explicit DistanceFromFacility(double weight = 6.0)
        : FlUnitConstraint("distance from facility", weight, /*hard=*/false)
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

#endif  // FL_CONSTRAINTS_DISTANCE_FROM_FACILITY_HPP
