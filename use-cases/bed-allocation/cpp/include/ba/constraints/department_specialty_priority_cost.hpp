#ifndef BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_PRIORITY_COST_HPP
#define BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_PRIORITY_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft, weight 10: departmentSpecialtyPriorityCost — every night a stay
// spends in a department that offers its specialty only at priority > 1,
// weighed by (priority - 1). Raw cost per bed = sum over its stays of
// (priority - 1) * nights, so 0 = every stay is in a department that offers
// its specialty at priority 1 (or not at all). Silent when the department
// does not offer the specialty: departmentSpecialtyCost already prices that
// case (and the Java provider would throw there). The Java quickstart calls
// it departmentSpecialtyNotFirstPriority.
struct DepartmentSpecialtyPriorityCost final : PerStayRoomConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DepartmentSpecialtyPriorityCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("departmentSpecialtyPriorityCost",
                                weight,
                                kDefaultLevel)
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

#endif  // BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_PRIORITY_COST_HPP
