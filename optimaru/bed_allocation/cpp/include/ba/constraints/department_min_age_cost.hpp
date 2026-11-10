#ifndef BA_CONSTRAINTS_DEPARTMENT_MIN_AGE_COST_HPP
#define BA_CONSTRAINTS_DEPARTMENT_MIN_AGE_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard, weight 100: departmentMinAgeCost — every night a patient younger
// than the department's minimum age spends there. Raw cost per bed = the
// nights of its stays below the minimum age of the bed's department (no
// minimum = silent), so 0 = every patient is old enough for their department.
struct DepartmentMinAgeCost final : PerStayRoomConstraint
{
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DepartmentMinAgeCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("departmentMinAgeCost", weight, kDefaultLevel)
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

#endif  // BA_CONSTRAINTS_DEPARTMENT_MIN_AGE_COST_HPP
