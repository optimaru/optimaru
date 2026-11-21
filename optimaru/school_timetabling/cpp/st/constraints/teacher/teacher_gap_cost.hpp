#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_GAP_COST_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_GAP_COST_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon {

// teacherGapCost — soft, weight 20. Raw cost = the idle periods wedged
// inside a teacher's day (last - first + 1 - count over the DISTINCT busy
// periods, via the shared gapOf), summed over (teacher, day); 0 = every
// teacher's day is one compact run.
struct TeacherGapCost final : TeacherDayMaskConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TeacherGapCost(double weight = kDefaultWeight,
                            std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("teacherGapCost", weight, kDefaultLevel, TeacherMaskHub::kGap,
                                   std::move(hub)) {}
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_GAP_COST_HPP
