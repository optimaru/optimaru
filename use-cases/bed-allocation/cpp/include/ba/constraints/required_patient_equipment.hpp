#ifndef BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_HPP
#define BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 50): requiredPatientEquipment — when the room misses
// at least one piece of required equipment, the penalty is weighed by the
// number of required pieces the room DOES have. That is the quickstart's
// exact, unit-test-pinned semantics (a room with none of the required
// equipment therefore costs 0), reproduced here verbatim.
struct RequiredPatientEquipment final : PerStayRoomConstraint
{
    explicit RequiredPatientEquipment(double weight = 50.0)
        : PerStayRoomConstraint("requiredPatientEquipment",
                                weight,
                                /*hard=*/true)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        std::int64_t present = 0;
        for (const int equipmentId : stay.requiredEquipmentIds)
        {
            if (plan.roomHasEquipment(roomIndex, equipmentId))
                ++present;
        }
        if (present
            >= static_cast<std::int64_t>(stay.requiredEquipmentIds.size()))
            return 0.0;
        return static_cast<double>(stay.nightCount() * present);
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_HPP
