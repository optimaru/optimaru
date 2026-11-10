#ifndef BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_COST_HPP
#define BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba {

// Hard, weight 50: femaleInMaleRoomCost — every night a female patient
// spends in a MALE_ONLY room. Raw cost per bed = the nights of its female
// stays when the bed's room is MALE_ONLY, so 0 = no female patient in a
// male-only room.
struct FemaleInMaleRoomCost final : PerStayRoomConstraint {
    static constexpr double kDefaultWeight = 50.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FemaleInMaleRoomCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("femaleInMaleRoomCost", weight, kDefaultLevel) {}
    double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const override {
        const Stay& stay = plan.stays[stayIndex];
        if (stay.patientGender != Gender::FEMALE ||
            plan.rooms[roomIndex].genderLimitation != GenderLimitation::MALE_ONLY) {
            return 0.0;
        }
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_COST_HPP
