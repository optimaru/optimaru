#ifndef ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_HPP
#define ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Hard (default): H3 class conflict — one point per pair of same-class
// lessons on one slot (C(n, 2) per (classGroup, slot) occupancy cell,
struct ClassConflict final : GroupableSlotConstraint
{
    explicit ClassConflict(double weight = 1.0)
        : GroupableSlotConstraint("H3 class conflict", weight, /*hard=*/true)
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
    bool supportsDelta() const override { return true; }
    double deltaCost(const Timetable &table,
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

#endif  // ST_CONSTRAINTS_SLOT_CLASS_CONFLICT_HPP
