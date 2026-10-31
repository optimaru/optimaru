#ifndef ES_CONSTRAINTS_ASSIGNMENT_CONSTRAINT_HPP
#define ES_CONSTRAINTS_ASSIGNMENT_CONSTRAINT_HPP

#include "es/constraints/common.hpp"

namespace es {

struct EmployeeAssignmentDeltaGroup;

// Marker base for constraints whose raw marginal cost is a projection of one
// shift joining/leaving an employee. The decomposed calculator discovers the
// common structural key and evaluates all present constraints as one group.
struct GroupableAssignmentConstraint : EsUnitConstraint {
    using EsUnitConstraint::EsUnitConstraint;
    const char* deltaGroupKey() const override;
    std::unique_ptr<optimaru::UnitConstraintDeltaGroup<EmployeeSchedule>> createDeltaGroup()
            const override;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_ASSIGNMENT_CONSTRAINT_HPP
