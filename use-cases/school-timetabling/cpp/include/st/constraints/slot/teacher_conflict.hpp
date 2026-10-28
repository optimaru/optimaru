#ifndef ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_HPP
#define ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Hard (default): H4 teacher conflict — one point per shared teacher per
// pair of lessons on one slot (team teaching occupies the slot once per
// teacher, exactly like the monolithic per-(teacher, slot) counts).
struct TeacherConflict final : GroupableSlotConstraint
{
    explicit TeacherConflict(double weight = 1.0)
        : GroupableSlotConstraint("H4 teacher conflict", weight, /*hard=*/true)
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
            for (std::size_t b = a + 1; b < members.size(); ++b)
            {
                pairs += detail::sharedTeachers(table.lessons[members[a]],
                                                table.lessons[members[b]]);
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
        const Lesson &lesson = table.lessons[entity];
        std::int64_t pairs = 0;
        for (int i : members)
        {
            if (i != entity)
                pairs += detail::sharedTeachers(lesson, table.lessons[i]);
        }
        return static_cast<double>(pairs);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_HPP
