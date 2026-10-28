#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::teacherFreeDay): S3 — S2
// restricted to the teacher's preferred free days.
struct TeacherFreeDay final : TeacherDayMaskConstraint
{
    explicit TeacherFreeDay(double weight = ObjectiveWeights{}.teacherFreeDay,
                            std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("S3 teacher free day",
                                   weight,
                                   /*hard=*/false,
                                   TeacherMaskHub::kFreeDay,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_FREE_DAY_HPP
