#ifndef BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_HPP
#define BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 50): femaleInMaleRoom — every night a female patient
// spends in a MALE_ONLY room.
struct FemaleInMaleRoom final : PerStayRoomConstraint
{
    explicit FemaleInMaleRoom(double weight = 50.0)
        : PerStayRoomConstraint("femaleInMaleRoom", weight, /*hard=*/true)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        if (stay.patientGender != Gender::FEMALE
            || plan.rooms[roomIndex].genderLimitation
                   != GenderLimitation::MALE_ONLY)
        {
            return 0.0;
        }
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_FEMALE_IN_MALE_ROOM_HPP
