#ifndef BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_COST_HPP
#define BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_COST_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard, weight 50: maleInFemaleRoomCost — every night a male patient spends
// in a FEMALE_ONLY room (the mirror of femaleInMaleRoomCost). Raw cost per
// bed = the nights of its male stays when the bed's room is FEMALE_ONLY, so
// 0 = no male patient in a female-only room.
struct MaleInFemaleRoomCost final : PerStayRoomConstraint
{
    static constexpr double kDefaultWeight = 50.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaleInFemaleRoomCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("maleInFemaleRoomCost", weight, kDefaultLevel)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        if (stay.patientGender != Gender::MALE
            || plan.rooms[roomIndex].genderLimitation
                   != GenderLimitation::FEMALE_ONLY)
        {
            return 0.0;
        }
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_COST_HPP
