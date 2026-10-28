#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::teacherWorkday): S2 — one point
// per (teacher, day) with any lesson.
struct TeacherWorkday final : TeacherDayMaskConstraint
{
    explicit TeacherWorkday(double weight = ObjectiveWeights{}.teacherWorkday,
                            std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("S2 teacher workday",
                                   weight,
                                   /*hard=*/false,
                                   TeacherMaskHub::kWorkday,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_WORKDAY_HPP
