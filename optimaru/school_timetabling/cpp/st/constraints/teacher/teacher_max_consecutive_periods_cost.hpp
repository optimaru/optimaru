#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_MAX_CONSECUTIVE_PERIODS_COST_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_MAX_CONSECUTIVE_PERIODS_COST_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon {

// teacherMaxConsecutivePeriodsCost — hard, weight 1. Raw cost = per
// (teacher, day): sum over the busy runs of max(0, run length -
// max_consecutive_periods), via the shared runExcessOf; 0 = no teacher
// teaches longer runs than allowed.
struct TeacherMaxConsecutivePeriodsCost final : TeacherDayMaskConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeacherMaxConsecutivePeriodsCost(double weight = kDefaultWeight,
                                              std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("teacherMaxConsecutivePeriodsCost", weight, kDefaultLevel,
                                   TeacherMaskHub::kMaxConsecutivePeriods, std::move(hub)) {}
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_MAX_CONSECUTIVE_PERIODS_COST_HPP
