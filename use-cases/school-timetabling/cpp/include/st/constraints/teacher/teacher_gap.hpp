#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_GAP_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_GAP_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::teacherGap): S1 — idle periods
// wedged inside a teacher's day (last - first + 1 - count over the DISTINCT
// busy periods, via the shared gapOf).
struct TeacherGap final : TeacherDayMaskConstraint
{
    explicit TeacherGap(double weight = ObjectiveWeights{}.teacherGap,
                        std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("S1 teacher gap",
                                   weight,
                                   /*hard=*/false,
                                   TeacherMaskHub::kGap,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_GAP_HPP
