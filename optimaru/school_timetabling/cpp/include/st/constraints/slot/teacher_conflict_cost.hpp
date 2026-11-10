#ifndef ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_COST_HPP
#define ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// teacherConflictCost — hard, weight 1. Raw cost per slot = one point per
// shared teacher per pair of lessons on it (team teaching occupies the slot
// once per teacher, exactly like the monolithic per-(teacher, slot) counts:
// C(n, 2) per (teacher, slot)), so 0 = no teacher stands in two lessons at
// once.
struct TeacherConflictCost final : GroupableSlotConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeacherConflictCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("teacherConflictCost", weight, kDefaultLevel)
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
            for (std::size_t b = a + 1; b < members.size(); ++b)
            {
                pairs += detail::sharedTeachers(table.lessons[members[a]],
                                                table.lessons[members[b]]);
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

#endif  // ST_CONSTRAINTS_SLOT_TEACHER_CONFLICT_COST_HPP
