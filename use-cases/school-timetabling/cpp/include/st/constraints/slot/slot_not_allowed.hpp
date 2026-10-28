#ifndef ST_CONSTRAINTS_SLOT_SLOT_NOT_ALLOWED_HPP
#define ST_CONSTRAINTS_SLOT_SLOT_NOT_ALLOWED_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Hard (default): H5 — one point per lesson assigned outside its allowedSlot
// table (calendar holes, forbidden slots, teacher/class unavailability are
// pre-baked into that table by the conversion layer).
struct SlotNotAllowed final : GroupableSlotConstraint
{
    explicit SlotNotAllowed(double weight = 1.0)
        : GroupableSlotConstraint("H5 slot not allowed", weight, /*hard=*/true)
    {
    }
    void onReset(const Timetable &table) override
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
    bool supportsDelta() const override { return true; }
    double deltaCost(const Timetable &table,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return table.lessons[entity].allowedSlot[unit] == 0 ? 1.0 : 0.0;
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_SLOT_NOT_ALLOWED_HPP
