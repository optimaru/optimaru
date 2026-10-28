#ifndef BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_HPP
#define BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft (default, weight 10): departmentSpecialty — every night a stay spends
// in a department that does not offer its specialty at all.
struct DepartmentSpecialty final : PerStayRoomConstraint
{
    explicit DepartmentSpecialty(double weight = 10.0)
        : PerStayRoomConstraint("departmentSpecialty", weight, /*hard=*/false)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        const int priority
            = stay.specialtyPriorityByDept[plan.rooms[roomIndex].department];
        return priority < 0 ? static_cast<double>(stay.nightCount()) : 0.0;
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_HPP
