#ifndef ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_HPP
#define ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Hard (default): H6 — per (roomType, slot): max(0, demand - capacity),
// summed over the room types (a merged lesson carries the SUM of its member
// courses' demands). Not a per-pair cost, but the marginal cost of one
// lesson joining/leaving is still exact from the other members' demand.
struct RoomCapacity final : GroupableSlotConstraint
{
    explicit RoomCapacity(double weight = 1.0)
        : GroupableSlotConstraint("H6 room capacity", weight, /*hard=*/true)
    {
    }
    void onReset(const Timetable &table) override
    {
        detail::requirePrepared(table);
    }
    double cost(const Timetable &table,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        std::int64_t excess = 0;
        for (int type = 0; type < table.roomTypeCount; ++type)
        {
            std::int64_t demand = 0;
            for (int i : members)
                demand += table.lessons[i].roomTypeDemand[type];
            excess += ScoreCalculator::max0(demand
                                            - table.roomCapacityByType[type]);
        }
        return static_cast<double>(excess);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const Timetable &table,
                     int /*unit*/,
                     const std::vector<int> &members,
                     int entity) const override
    {
        const Lesson &lesson = table.lessons[entity];
        std::int64_t delta = 0;
        for (int type = 0; type < table.roomTypeCount; ++type)
        {
            const int demand = lesson.roomTypeDemand[type];
            if (demand == 0)
                continue;
            std::int64_t others = 0;
            for (int i : members)
            {
                if (i != entity)
                    others += table.lessons[i].roomTypeDemand[type];
            }
            const int capacity = table.roomCapacityByType[type];
            delta += ScoreCalculator::max0(others + demand - capacity)
                     - ScoreCalculator::max0(others - capacity);
        }
        return static_cast<double>(delta);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_HPP
