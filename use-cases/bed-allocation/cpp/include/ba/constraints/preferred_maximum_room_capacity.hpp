#ifndef BA_CONSTRAINTS_PREFERRED_MAXIMUM_ROOM_CAPACITY_HPP
#define BA_CONSTRAINTS_PREFERRED_MAXIMUM_ROOM_CAPACITY_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Soft (default, weight 8): preferredMaximumRoomCapacity — every night a
// patient spends in a room larger than they prefer.
struct PreferredMaximumRoomCapacity final : PerStayRoomConstraint
{
    explicit PreferredMaximumRoomCapacity(double weight = 8.0)
        : PerStayRoomConstraint("preferredMaximumRoomCapacity",
                                weight,
                                /*hard=*/false)
    {
    }
    double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const override
    {
        const Stay &stay = plan.stays[stayIndex];
        if (!stay.patientPreferredMaximumRoomCapacity
            || *stay.patientPreferredMaximumRoomCapacity
                   >= plan.rooms[roomIndex].capacity)
        {
            return 0.0;
        }
        return static_cast<double>(stay.nightCount());
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_PREFERRED_MAXIMUM_ROOM_CAPACITY_HPP
