#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_LOAD_LIMIT_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_LOAD_LIMIT_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// Hard (default): H9 — per (teacher, day): max(0, distinct busy periods -
// maxPerDay), plus per teacher: max(0, weekly distinct busy periods -
// maxPerWeek); one component, matching the single H9 of the CP-SAT
// reference (maxPerWeek < 0 = unlimited).
struct TeacherLoadLimit final : TeacherDayMaskConstraint
{
    explicit TeacherLoadLimit(double weight = 1.0,
                              std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("H9 teacher load limit",
                                   weight,
                                   /*hard=*/true,
                                   TeacherMaskHub::kLoadLimit,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_LOAD_LIMIT_HPP
