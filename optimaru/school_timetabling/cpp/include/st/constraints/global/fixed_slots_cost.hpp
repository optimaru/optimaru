#ifndef ST_CONSTRAINTS_GLOBAL_FIXED_SLOTS_COST_HPP
#define ST_CONSTRAINTS_GLOBAL_FIXED_SLOTS_COST_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon {

// fixedSlotsCost — hard, weight 1, NOT level-flippable (structural). The
// CP-SAT reference's rule for the fixed_slots field: a course's lessons off its fixed
// (day, period) slots. Raw cost = 0 by construction in this domain: the
// conversion layer turns every fixed slot into a pinned Lesson holding that
// slot (Lesson::pinned), and the engine never moves a pinned lesson, so no
// lesson can leave its fixed slot. (A pinned lesson without any slot is an
// unplaced period, which unassignedLessonsCost charges.)
//
// It is a real component so the catalog, the weight/level/exponent knobs and
// constraint_costs() cover fixed_slots_cost like every other constraint:
// level='soft' is refused (soft() throws), level='hard' is a no-op, a weight or
// an exponent are accepted and scale a zero.
struct FixedSlotsCost final : MemberChangeGlobalConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FixedSlotsCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("fixedSlotsCost", weight, kDefaultLevel) {
        level_flippable = false;  // structural: pinned lessons, never scored
    }
    void reset(const Timetable& table, const std::vector<std::vector<int>>& /*members*/) override {
        detail::requirePrepared(table);
    }
    void on_member_changed(const Timetable& /*table*/, int /*unit*/,
                           const std::vector<int>& /*members*/, int /*entity*/,
                           bool /*joined*/) override {}
    double cost() const override { return 0.0; }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_FIXED_SLOTS_COST_HPP
