#ifndef BA_CONSTRAINTS_PREFERRED_MAX_ROOM_CAPACITY_COST_HPP
#define BA_CONSTRAINTS_PREFERRED_MAX_ROOM_CAPACITY_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba {

// Soft, weight 8: preferredMaxRoomCapacityCost — every night a patient
// spends in a room larger than they prefer (a preference, not a capacity
// overflow). Raw cost per bed = the nights of its stays whose
// patientPreferredMaximumRoomCapacity is below the room's capacity (no
// preference = silent), so 0 = every patient's room is small enough.
struct PreferredMaxRoomCapacityCost final : PerStayRoomConstraint {
    static constexpr double kDefaultWeight = 8.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit PreferredMaxRoomCapacityCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("preferredMaxRoomCapacityCost", weight, kDefaultLevel) {}
    double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const override {
        const Stay& stay = plan.stays[stayIndex];
        if (!stay.patientPreferredMaximumRoomCapacity ||
            *stay.patientPreferredMaximumRoomCapacity >= plan.rooms[roomIndex].capacity) {
            return 0.0;
        }
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_PREFERRED_MAX_ROOM_CAPACITY_COST_HPP
