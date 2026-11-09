#ifndef ST_CONSTRAINTS_SLOT_DISALLOWED_SLOT_COST_HPP
#define ST_CONSTRAINTS_SLOT_DISALLOWED_SLOT_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// disallowedSlotCost — hard, weight 1. Raw cost per slot = the lessons on it
// that their allowedSlot table forbids (calendar holes, course
// forbidden_slots, teacher and class unavailability are pre-baked into that
// table by the conversion layer), so 0 = every lesson sits on a time it may
// use. Engine-only: the CP-SAT reference never creates a variable for a
// forbidden time.
struct DisallowedSlotCost final : GroupableSlotConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DisallowedSlotCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("disallowedSlotCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const Timetable &table) override
    {
        detail::requirePrepared(table);
    }
    double cost(const Timetable &table,
                int unit,
                const std::vector<int> &members) const override
    {
        std::int64_t hits = 0;
        for (int i : members)
        {
            if (table.lessons[i].allowedSlot[unit] == 0)
                ++hits;
        }
        return static_cast<double>(hits);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const Timetable &table,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return table.lessons[entity].allowedSlot[unit] == 0 ? 1.0 : 0.0;
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_DISALLOWED_SLOT_COST_HPP
