#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_COST_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_COST_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// teacherFreeDayCost — soft, weight 30. Raw cost = the worked requested free
// days: (teacher, day) pairs with any lesson on a day the teacher asked to
// keep free (preferred_free_days), i.e. teacherWorkdayCost restricted to
// those days; 0 = every requested free day stays free.
struct TeacherFreeDayCost final : TeacherDayMaskConstraint
{
    static constexpr double kDefaultWeight = 30.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TeacherFreeDayCost(double weight = kDefaultWeight,
                                std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("teacherFreeDayCost",
                                   weight,
                                   kDefaultLevel,
                                   TeacherMaskHub::kFreeDay,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_COST_HPP
