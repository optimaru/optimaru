#ifndef BA_CONSTRAINTS_DEPARTMENT_MAX_AGE_COST_HPP
#define BA_CONSTRAINTS_DEPARTMENT_MAX_AGE_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard, weight 100: departmentMaxAgeCost — every night a patient older than
// the department's maximum age spends there. Raw cost per bed = the nights of
// its stays above the maximum age of the bed's department (no maximum =
// silent), so 0 = every patient is young enough for their department.
struct DepartmentMaxAgeCost final : PerStayRoomConstraint
{
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DepartmentMaxAgeCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("departmentMaxAgeCost", weight, kDefaultLevel)
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

#endif  // BA_CONSTRAINTS_DEPARTMENT_MAX_AGE_COST_HPP
