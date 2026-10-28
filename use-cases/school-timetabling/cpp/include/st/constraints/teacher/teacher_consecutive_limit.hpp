#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_CONSECUTIVE_LIMIT_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_CONSECUTIVE_LIMIT_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon
{

// Hard (default): H10 — per (teacher, day): sum over busy runs of
// max(0, run - maxConsecutive), via the shared runExcessOf.
struct TeacherConsecutiveLimit final : TeacherDayMaskConstraint
{
    explicit TeacherConsecutiveLimit(double weight = 1.0,
                                     std::shared_ptr<TeacherMaskHub> hub
                                     = nullptr)
        : TeacherDayMaskConstraint("H10 teacher consecutive limit",
                                   weight,
                                   /*hard=*/true,
                                   TeacherMaskHub::kConsecutive,
                                   std::move(hub))
    {
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_CONSECUTIVE_LIMIT_HPP
