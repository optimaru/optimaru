#ifndef BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_HPP
#define BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 50): maleInFemaleRoom — every night a male patient
// spends in a FEMALE_ONLY room.
struct MaleInFemaleRoom final : PerStayRoomConstraint
{
    explicit MaleInFemaleRoom(double weight = 50.0)
        : PerStayRoomConstraint("maleInFemaleRoom", weight, /*hard=*/true)
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

#endif  // BA_CONSTRAINTS_MALE_IN_FEMALE_ROOM_HPP
