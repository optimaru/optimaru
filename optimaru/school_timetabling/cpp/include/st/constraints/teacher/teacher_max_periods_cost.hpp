#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_MAX_PERIODS_COST_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_MAX_PERIODS_COST_HPP

#include "st/constraints/teacher/teacher_mask_hub.hpp"

namespace st::canon {

// teacherMaxPeriodsCost — hard, weight 1. Raw cost = per (teacher, day):
// max(0, distinct busy periods - max_periods_per_day), plus per teacher:
// max(0, weekly distinct busy periods - max_periods_per_week)
// (maxPerWeek < 0 = unlimited). One component, matching the single
// teacher-workload rule of the CP-SAT reference; 0 = every teacher within
// both caps.
struct TeacherMaxPeriodsCost final : TeacherDayMaskConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TeacherMaxPeriodsCost(double weight = kDefaultWeight,
                                   std::shared_ptr<TeacherMaskHub> hub = nullptr)
        : TeacherDayMaskConstraint("teacherMaxPeriodsCost", weight, kDefaultLevel,
                                   TeacherMaskHub::kMaxPeriods, std::move(hub)) {}
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_MAX_PERIODS_COST_HPP
