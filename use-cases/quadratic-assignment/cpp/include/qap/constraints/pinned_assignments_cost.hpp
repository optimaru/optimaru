#ifndef QAP_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP
#define QAP_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP

#include "qap/constraints/common.hpp"

namespace qap
{

// Hard, weight 1: pinnedAssignmentsCost — fixed_assignments that are not kept.
// Raw cost per location = 1 when the location is fixed to a facility that is
// not standing there (also when the location is empty), else 0; an unfixed
// location is always 0, so 0 = every pin is kept. Two locations fixed to the
// same facility cost at least 1 (the facility can stand at only one of them).
// It lists one term per location with the unfixed ones at 0, so the shaped
// total equals one 0/1 term per fixed location for every exponent.
//
// Delta contract: a facility appears at most once among a location's members,
// so the cost is 1 - [fixed facility among the members] and the marginal cost
// of one member is -1 for the fixed facility, 0 for any other.
struct PinnedAssignmentsCost final : QapUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit PinnedAssignmentsCost(double weight = kDefaultWeight)
        : QapUnitConstraint("pinnedAssignmentsCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const QuadraticAssignmentProblem &problem) override
    {
        detail::requirePrepared(problem);
        scope_ = &problem.fixedLocations;
    }
    double cost(const QuadraticAssignmentProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        const int pinned = problem.fixedFacilityOf[unit];
        if (pinned < 0)
            return 0.0;
        for (int facility : members)
        {
            if (facility == pinned)
                return 0.0;
        }
        return 1.0;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const QuadraticAssignmentProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return entity == problem.fixedFacilityOf[unit] ? -1.0 : 0.0;
    }
    const std::vector<int> *scoped_units() const override { return scope_; }

private:
    const std::vector<int> *scope_ = nullptr;
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP
