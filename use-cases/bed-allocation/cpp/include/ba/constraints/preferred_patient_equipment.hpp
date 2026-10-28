#ifndef BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_HPP
#define BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft (default, weight 50): preferredPatientEquipment — every night, weighed
// by the number of preferred equipment pieces the room lacks.
struct PreferredPatientEquipment final : PerStayRoomConstraint
{
    explicit PreferredPatientEquipment(double weight = 50.0)
        : PerStayRoomConstraint("preferredPatientEquipment",
                                weight,
                                /*hard=*/false)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        std::int64_t missing = 0;
        for (const int equipmentId : stay.preferredEquipmentIds)
        {
            if (!plan.roomHasEquipment(roomIndex, equipmentId))
                ++missing;
        }
        return static_cast<double>(stay.nightCount() * missing);
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_HPP
