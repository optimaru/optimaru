#ifndef BA_CONSTRAINTS_DEPARTMENT_MAXIMUM_AGE_HPP
#define BA_CONSTRAINTS_DEPARTMENT_MAXIMUM_AGE_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 100): departmentMaximumAge — every night a patient
// older than the department's maximum age spends there.
struct DepartmentMaximumAge final : PerStayRoomConstraint
{
    explicit DepartmentMaximumAge(double weight = 100.0)
        : PerStayRoomConstraint("departmentMaximumAge", weight, /*hard=*/true)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        const Department &department
            = plan.departments[plan.rooms[roomIndex].department];
        if (!department.maximumAge || stay.patientAge <= *department.maximumAge)
            return 0.0;
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DEPARTMENT_MAXIMUM_AGE_HPP
