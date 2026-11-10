#ifndef ST_CONSTRAINTS_GLOBAL_PARALLEL_GROUPS_COST_HPP
#define ST_CONSTRAINTS_GLOBAL_PARALLEL_GROUPS_COST_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// parallelGroupsCost — hard, weight 1, NOT level-flippable (structural). The
// CP-SAT reference's rule for the parallel_group field: the sections of one
// parallel_group (a class split into groups taught at the same time) placed
// at different times. Raw cost = 0 by construction in this domain: the
// conversion layer merges each parallel group into ONE merged Lesson per
// weekly period (teachers = the union, room demand = the sum, one class
// occupancy), so its sections cannot be split.
//
// It is a real component so the catalog, the weight/level/exponent knobs and
// constraint_costs() cover parallel_groups_cost like every other constraint:
// level='soft' is refused (soft() throws), level='hard' is a no-op, a weight or
// an exponent are accepted and scale a zero.
struct ParallelGroupsCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ParallelGroupsCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint(
              "parallelGroupsCost", weight, kDefaultLevel)
    {
        level_flippable = false;  // structural: merged lessons, never split
    }
    void reset(const Timetable &table,
               const std::vector<std::vector<int>> & /*members*/) override
    {
        detail::requirePrepared(table);
    }
    void on_member_changed(const Timetable & /*table*/,
                         int /*unit*/,
                         const std::vector<int> & /*members*/,
                         int /*entity*/,
                         bool /*joined*/) override
    {
    }
    double cost() const override { return 0.0; }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_PARALLEL_GROUPS_COST_HPP
