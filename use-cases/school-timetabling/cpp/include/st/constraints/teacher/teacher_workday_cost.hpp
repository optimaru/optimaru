#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_COST_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_COST_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// teacherWorkdayCost — soft, weight 3. Raw cost = the number of
// (teacher, day) pairs with any lesson: one point per day a teacher has to
// come in. 0 only for a timetable without lessons — the term pulls each
// teacher's lessons onto fewer days.
struct TeacherWorkdayCost final : TeacherDayMaskConstraint
{
    static constexpr double kDefaultWeight = 3.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TeacherWorkdayCost(double weight = kDefaultWeight,
                                std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("teacherWorkdayCost",
                                   weight,
                                   kDefaultLevel,
                                   TeacherMaskHub::kWorkday,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_COST_HPP
