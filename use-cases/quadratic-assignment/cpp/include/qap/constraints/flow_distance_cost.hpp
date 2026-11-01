#ifndef QAP_CONSTRAINTS_FLOW_DISTANCE_COST_HPP
#define QAP_CONSTRAINTS_FLOW_DISTANCE_COST_HPP

#include <vector>

#include "qap/constraints/common.hpp"

namespace qap
{

// Soft, weight 1: flowDistanceCost — the quadratic objective. Raw cost = sum
// over assigned facilities a, b of flows[a][b] * distances[loc a][loc b],
// including a == b (the diagonal term flows[a][a] * distances[l][l]); on a
// bijection this is the 0.1.x sum over location pairs i, j of
// distances[i][j] * flows[p_i][p_j]. One number for the whole assignment (a
// GlobalConstraint, so the exponent shapes the total). Flows and distances are
// not validated >= 0, so the total may be negative (scored as a bonus).
//
// Member-change fast path with its own location mirror: a facility f leaving
// location l retracts contrib(f, l), then leaves the mirror; joining enters the
// mirror, then adds contrib(f, l), where
//   contrib(f, l) = sum over assigned g != f of
//                   flows[f][g] * distances[l][loc g] + flows[g][f] * distances[loc g][l]
//                 + flows[f][f] * distances[l][l].
// The total is a function of the mirror alone, so the transient states inside
// a swap (two facilities on one location) stay exact. O(n) per event, four
// events per swap.
struct FlowDistanceCost final : QapMemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit FlowDistanceCost(double weight = kDefaultWeight)
        : QapMemberChangeGlobalConstraint("flowDistanceCost", weight, kDefaultLevel)
    {
    }
    void reset(const QuadraticAssignmentProblem &problem,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(problem);
        location_.assign(static_cast<std::size_t>(problem.facilityCount()), -1);
        for (std::size_t location = 0; location < membersPerUnit.size(); ++location)
        {
            for (int facility : membersPerUnit[location])
                location_[facility] = static_cast<int>(location);
        }
        total_ = 0;
        const int n = problem.facilityCount();
        for (int a = 0; a < n; ++a)
        {
            if (location_[a] < 0)
                continue;
            for (int b = 0; b < n; ++b)
            {
                if (location_[b] >= 0)
                    total_ += problem.flowOf(a, b) * problem.distanceOf(location_[a], location_[b]);
            }
        }
    }
    void onMemberChanged(const QuadraticAssignmentProblem &problem,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        if (joined)
        {
            location_[entity] = unit;
            total_ += contribution(problem, entity, unit);
        }
        else
        {
            total_ -= contribution(problem, entity, unit);
            location_[entity] = -1;
        }
    }
    double cost() const override { return static_cast<double>(total_); }

private:
    std::int64_t contribution(const QuadraticAssignmentProblem &problem, int facility, int location) const
    {
        std::int64_t sum = problem.flowOf(facility, facility) * problem.distanceOf(location, location);
        const int n = problem.facilityCount();
        for (int other = 0; other < n; ++other)
        {
            const int at = location_[other];
            if (other == facility || at < 0)
                continue;
            sum += problem.flowOf(facility, other) * problem.distanceOf(location, at)
                   + problem.flowOf(other, facility) * problem.distanceOf(at, location);
        }
        return sum;
    }

    std::vector<int> location_;  // per facility, the mirror of the members
    std::int64_t total_ = 0;
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_FLOW_DISTANCE_COST_HPP
