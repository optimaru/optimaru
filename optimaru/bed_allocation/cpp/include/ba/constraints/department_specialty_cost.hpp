#ifndef BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_COST_HPP
#define BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba {

// Soft, weight 10: departmentSpecialtyCost — every night a stay spends in a
// department that does not offer its specialty at all. Raw cost per bed = the
// nights of its stays whose specialty the bed's department does not list, so
// 0 = every stay is in a department offering its specialty.
struct DepartmentSpecialtyCost final : PerStayRoomConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DepartmentSpecialtyCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("departmentSpecialtyCost", weight, kDefaultLevel) {}
    double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const override {
        const Stay& stay = plan.stays[stayIndex];
        const int priority = stay.specialtyPriorityByDept[plan.rooms[roomIndex].department];
        return priority < 0 ? static_cast<double>(stay.nightCount()) : 0.0;
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DEPARTMENT_SPECIALTY_COST_HPP
