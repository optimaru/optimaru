#ifndef BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_COST_HPP
#define BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba {

// Hard, weight 50: requiredPatientEquipmentCost — when the room misses at
// least one piece of required equipment, the penalty is weighed by the
// number of required pieces the room DOES have. That is the intended,
// unit-test-pinned semantics (a room with none of the required
// equipment therefore costs 0); it is NOT a missing-items count like
// RequiredSkillCost.
// Raw cost per bed = sum over its stays whose room misses a required item of
// nights * (required items present), so 0 = every stay has all of its
// required equipment, or none of it.
struct RequiredPatientEquipmentCost final : PerStayRoomConstraint {
    static constexpr double kDefaultWeight = 50.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequiredPatientEquipmentCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("requiredPatientEquipmentCost", weight, kDefaultLevel) {}
    double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const override {
        const Stay& stay = plan.stays[stayIndex];
        std::int64_t present = 0;
        for (const int equipmentId : stay.requiredEquipmentIds) {
            if (plan.roomHasEquipment(roomIndex, equipmentId)) ++present;
        }
        if (present >= static_cast<std::int64_t>(stay.requiredEquipmentIds.size())) return 0.0;
        return static_cast<double>(stay.nightCount() * present);
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_REQUIRED_PATIENT_EQUIPMENT_COST_HPP
