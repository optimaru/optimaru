#ifndef BA_CONSTRAINTS_DIFFERENT_GENDER_IN_SAME_GENDER_ROOM_HPP
#define BA_CONSTRAINTS_DIFFERENT_GENDER_IN_SAME_GENDER_ROOM_HPP

#include <algorithm>

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 1000): differentGenderInSameGenderRoomInSameNight —
// one point per night a male and a female patient share a SAME_GENDER room.
//
// Global because its pairs share a ROOM while the calculator's unit is a BED,
// and a room holds several beds. It maintains the occupant list of every
// SAME_GENDER room and folds the marginal pair sum on each join/leave, which
// is exactly reversible and therefore safe on the member-change fast path.
struct DifferentGenderInSameGenderRoomInSameNight final
    : MemberChangeGlobalConstraint
{
    explicit DifferentGenderInSameGenderRoomInSameNight(double weight = 1000.0)
        : MemberChangeGlobalConstraint(
              "differentGenderInSameGenderRoomInSameNight",
              weight,
              /*hard=*/true)
    {
    }

    void reset(const BedPlan &plan,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(plan);
        occupants_.assign(plan.rooms.size(), {});
        total_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            const int room = roomOfSameGenderBed(plan, static_cast<int>(unit));
            if (room < 0)
                continue;
            for (int stayIndex : membersPerUnit[unit])
            {
                total_ += pairNights(plan, occupants_[room], stayIndex);
                occupants_[room].push_back(stayIndex);
            }
        }
    }

    void onMemberChanged(const BedPlan &plan,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        const int room = roomOfSameGenderBed(plan, unit);
        if (room < 0)
            return;
        std::vector<int> &occupants = occupants_[room];
        if (joined)
        {
            total_ += pairNights(plan, occupants, entity);
            occupants.push_back(entity);
            return;
        }
        // Swap-remove first, so the marginal sum sees exactly the OTHER
        // occupants — the mirror image of the join above.
        const auto it = std::find(occupants.begin(), occupants.end(), entity);
        if (it == occupants.end())
            return;
        *it = occupants.back();
        occupants.pop_back();
        total_ -= pairNights(plan, occupants, entity);
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    // The room of `value` when that bed stands in a SAME_GENDER room, else -1
    // (the sentinel and every other limitation are silent here).
    static int roomOfSameGenderBed(const BedPlan &plan, int value)
    {
        if (!detail::isRealBed(plan, value))
            return -1;
        const int room = plan.beds[value].room;
        return plan.rooms[room].genderLimitation
                       == GenderLimitation::SAME_GENDER
                   ? room
                   : -1;
    }

    // Shared nights between `stayIndex` and every mixed-gender occupant.
    static std::int64_t pairNights(const BedPlan &plan,
                                   const std::vector<int> &occupants,
                                   int stayIndex)
    {
        const Stay &stay = plan.stays[stayIndex];
        std::int64_t nights = 0;
        for (int other : occupants)
        {
            const Stay &occupant = plan.stays[other];
            if (occupant.patientGender != stay.patientGender)
                nights += stay.sameNightCount(occupant);
        }
        return nights;
    }

    std::vector<std::vector<int>> occupants_;  // per SAME_GENDER room
    std::int64_t total_ = 0;
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_DIFFERENT_GENDER_IN_SAME_GENDER_ROOM_HPP
