#ifndef QAP_CONSTRAINTS_FORBIDDEN_ASSIGNMENTS_COST_HPP
#define QAP_CONSTRAINTS_FORBIDDEN_ASSIGNMENTS_COST_HPP

#include "qap/constraints/common.hpp"

namespace qap
{

// Hard, weight 1: forbiddenAssignmentsCost — facilities standing at a location
// a forbidden_assignments row forbids them. Raw cost per location = the number
// of its facilities forbidden there (0 or 1 in a bijection), so 0 = no
// forbidden pair is used. A pair listed twice counts once (the rows are a set),
// exactly like the 0.1.x Python scorer and the CP-SAT reference.
//
// A pure sum of per-member terms, so the exact delta is one mask lookup.
struct ForbiddenAssignmentsCost final : QapUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ForbiddenAssignmentsCost(double weight = kDefaultWeight)
        : QapUnitConstraint("forbiddenAssignmentsCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const QuadraticAssignmentProblem &problem) override
    {
        detail::requirePrepared(problem);
        scope_ = &problem.forbiddenLocations;
    }
    double cost(const QuadraticAssignmentProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        int count = 0;
        for (int facility : members)
            count += problem.isForbidden(facility, unit) ? 1 : 0;
        return static_cast<double>(count);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const QuadraticAssignmentProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return problem.isForbidden(entity, unit) ? 1.0 : 0.0;
    }
    const std::vector<int> *scoped_units() const override { return scope_; }

private:
    const std::vector<int> *scope_ = nullptr;
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_FORBIDDEN_ASSIGNMENTS_COST_HPP
