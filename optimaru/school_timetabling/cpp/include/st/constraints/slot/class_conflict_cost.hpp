#ifndef ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_COST_HPP
#define ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// classConflictCost — hard, weight 1. Raw cost per slot = the number of
// pairs of same-class lessons on it (C(n, 2) per (classGroup, slot)
// occupancy cell; a merged parallel group is one occupancy unit), so 0 = no
// class has two lessons at once.
struct ClassConflictCost final : GroupableSlotConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ClassConflictCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("classConflictCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const Timetable &table) override
    {
        detail::requirePrepared(table);
    }
    double cost(const Timetable &table,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        std::int64_t pairs = 0;
        for (std::size_t a = 0; a < members.size(); ++a)
        {
            const int classGroup = table.lessons[members[a]].classGroupId;
            for (std::size_t b = a + 1; b < members.size(); ++b)
            {
                if (table.lessons[members[b]].classGroupId == classGroup)
                    ++pairs;
            }
        }
        return static_cast<double>(pairs);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const Timetable &table,
                     int /*unit*/,
                     const std::vector<int> &members,
                     int entity) const override
    {
        const int classGroup = table.lessons[entity].classGroupId;
        std::int64_t pairs = 0;
        for (int i : members)
        {
            if (i != entity && table.lessons[i].classGroupId == classGroup)
                ++pairs;
        }
        return static_cast<double>(pairs);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_COST_HPP
