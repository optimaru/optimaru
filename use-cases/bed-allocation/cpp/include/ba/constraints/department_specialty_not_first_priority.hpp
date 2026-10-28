#ifndef BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_NOT_FIRST_PRIORITY_HPP
#define BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_NOT_FIRST_PRIORITY_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft (default, weight 10): departmentSpecialtyNotFirstPriority — every
// night a stay spends in a department that offers its specialty only at
// priority > 1, weighed by (priority - 1). Silent when the department does
// not offer the specialty at all: departmentSpecialty already prices that
// case (and the Java provider would throw there).
struct DepartmentSpecialtyNotFirstPriority final : PerStayRoomConstraint
{
    explicit DepartmentSpecialtyNotFirstPriority(double weight = 10.0)
        : PerStayRoomConstraint("departmentSpecialtyNotFirstPriority",
                                weight,
                                /*hard=*/false)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        const int priority
            = stay.specialtyPriorityByDept[plan.rooms[roomIndex].department];
        if (priority <= 1)
            return 0.0;
        return static_cast<double>((priority - 1) * stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_NOT_FIRST_PRIORITY_HPP
