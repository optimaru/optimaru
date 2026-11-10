#ifndef ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_COST_HPP
#define ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon {

// roomCapacityCost — hard, weight 1. Raw cost per slot = sum over the
// special-room types of max(0, demand - owned rooms) (a merged lesson
// carries the SUM of its member courses' demands), so 0 = every special-room
// lesson finds a room of its type. Not a per-pair cost, but the marginal
// cost of one lesson joining/leaving is still exact from the other members'
// demand.
struct RoomCapacityCost final : GroupableSlotConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RoomCapacityCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("roomCapacityCost", weight, kDefaultLevel) {}
    void on_reset(const Timetable& table) override { detail::requirePrepared(table); }
    double cost(const Timetable& table, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t excess = 0;
        for (int type = 0; type < table.roomTypeCount; ++type) {
            std::int64_t demand = 0;
            for (int i : members) demand += table.lessons[i].roomTypeDemand[type];
            excess += ScoreCalculator::max0(demand - table.roomCapacityByType[type]);
        }
        return static_cast<double>(excess);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const Timetable& table, int /*unit*/, const std::vector<int>& members,
                      int entity) const override {
        const Lesson& lesson = table.lessons[entity];
        std::int64_t delta = 0;
        for (int type = 0; type < table.roomTypeCount; ++type) {
            const int demand = lesson.roomTypeDemand[type];
            if (demand == 0) continue;
            std::int64_t others = 0;
            for (int i : members) {
                if (i != entity) others += table.lessons[i].roomTypeDemand[type];
            }
            const int capacity = table.roomCapacityByType[type];
            delta += ScoreCalculator::max0(others + demand - capacity) -
                     ScoreCalculator::max0(others - capacity);
        }
        return static_cast<double>(delta);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_ROOM_CAPACITY_COST_HPP
