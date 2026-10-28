#ifndef BA_CONSTRAINTS_DEPARTMENT_MINIMUM_AGE_HPP
#define BA_CONSTRAINTS_DEPARTMENT_MINIMUM_AGE_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 100): departmentMinimumAge — every night a patient
// younger than the department's minimum age spends there.
struct DepartmentMinimumAge final : PerStayRoomConstraint
{
    explicit DepartmentMinimumAge(double weight = 100.0)
        : PerStayRoomConstraint("departmentMinimumAge", weight, /*hard=*/true)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        const Department &department
            = plan.departments[plan.rooms[roomIndex].department];
        if (!department.minimumAge || stay.patientAge >= *department.minimumAge)
            return 0.0;
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DEPARTMENT_MINIMUM_AGE_HPP
