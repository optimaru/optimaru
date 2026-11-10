#ifndef BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_COST_HPP
#define BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft, weight 50: preferredPatientEquipmentCost — every night, weighed by
// the number of preferred equipment pieces the room lacks. Raw cost per bed =
// sum over its stays of nights * (preferred items missing from the room), so
// 0 = every stay's room has all of its preferred equipment.
struct PreferredPatientEquipmentCost final : PerStayRoomConstraint
{
    static constexpr double kDefaultWeight = 50.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit PreferredPatientEquipmentCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("preferredPatientEquipmentCost",
                                weight,
                                kDefaultLevel)
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

#endif  // BA_CONSTRAINTS_PREFERRED_PATIENT_EQUIPMENT_COST_HPP
