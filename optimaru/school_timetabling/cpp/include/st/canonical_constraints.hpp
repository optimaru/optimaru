#ifndef ST_CANONICAL_CONSTRAINTS_HPP
#define ST_CANONICAL_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Canonical constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, <subject>_cost.hpp, grouped by
// what they share:
//   slot/     the per-slot family. They stay together because the calculator
//             compiles every present member into ONE SlotDeltaGroup evaluate
//             per member change — that structural fast path is why the plugin
//             path can stand next to the monolithic calculator at all.
//   global/   the rules whose state spans slots (per course, block group),
//             the unassigned-lesson count and the two structural constraints.
//   teacher/  the per-(teacher, day) mask family, sharing ONE TeacherMaskHub
//             projection per assembly instead of five private copies.
// common.hpp holds what all of them need; class_gap_cost.hpp keeps its own
// class-keyed mask instance.
//
// Including this header still gives the whole set. Adding a component is a
// file plus a line here (and a line in buildDefaultSet below when it belongs
// to the default assembly).

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "st/constraints/class_gap_cost.hpp"
#include "st/constraints/common.hpp"
#include "st/constraints/global/block_adjacency_cost.hpp"
#include "st/constraints/global/course_max_periods_per_day_cost.hpp"
#include "st/constraints/global/fixed_slots_cost.hpp"
#include "st/constraints/global/global_constraint.hpp"
#include "st/constraints/global/parallel_groups_cost.hpp"
#include "st/constraints/global/unassigned_lessons_cost.hpp"
#include "st/constraints/slot/avoided_period_cost.hpp"
#include "st/constraints/slot/class_conflict_cost.hpp"
#include "st/constraints/slot/disallowed_slot_cost.hpp"
#include "st/constraints/slot/non_preferred_period_cost.hpp"
#include "st/constraints/slot/room_capacity_cost.hpp"
#include "st/constraints/slot/slot_constraint.hpp"
#include "st/constraints/slot/slot_delta_group.hpp"
#include "st/constraints/slot/teacher_conflict_cost.hpp"
#include "st/constraints/teacher/teacher_free_day_cost.hpp"
#include "st/constraints/teacher/teacher_gap_cost.hpp"
#include "st/constraints/teacher/teacher_mask_hub.hpp"
#include "st/constraints/teacher/teacher_max_consecutive_periods_cost.hpp"
#include "st/constraints/teacher/teacher_max_periods_cost.hpp"
#include "st/constraints/teacher/teacher_workday_cost.hpp"

namespace st::canon {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full canonical set, every component at its kDefaultWeight /
// kDefaultLevel: nine hard constraints at weight 1, the six soft ones at
// 20/3/30/10/5/1, and the two structural ones (always 0). It scores
// bit-for-bit like ScoreCalculator::score and the monolithic
// incremental calculator at their default ObjectiveWeights.
//
// `defaultConstraints()` hands the set back as a ConstraintAssembly, so a
// caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("teacherGapCost", 40)
//         .drop("teacherWorkdayCost")
//         .soft("unassignedLessonsCost")   // partial timetabling
//         .into(calc);
//
// The five teacher-mask members share ONE TeacherMaskHub: `maskHub`, or a
// fresh one when it is null. Pass the hub of the calculator the components
// will join (sharedTeacherMaskHub) when adding to a calculator that already
// holds family members; the hub must only ever be shared by constraints of
// one calculator.
namespace detail {

struct DefaultSet {
    ConstraintAssembly assembly;
    std::vector<std::string> order;  // registry names, in emplace order
};

// The one list of the default set. Its emplace order is the catalog order —
// the CP-SAT reference's numbering (unassigned lessons, the conflicts, ...,
// the six soft terms), then the two structural constraints — which
// ConstraintAssembly itself does not keep (it reports its unit constraints
// before its global ones), so the order is recorded alongside.
inline DefaultSet buildDefaultSet(std::shared_ptr<TeacherMaskHub> maskHub) {
    if (maskHub == nullptr) maskHub = std::make_shared<TeacherMaskHub>();
    DefaultSet set;
    auto add = [&set](auto component) {
        set.order.push_back(component->name);
        set.assembly.add(std::move(component));
    };
    add(std::make_unique<UnassignedLessonsCost>());
    add(std::make_unique<ClassConflictCost>());
    add(std::make_unique<TeacherConflictCost>());
    add(std::make_unique<DisallowedSlotCost>());
    add(std::make_unique<RoomCapacityCost>());
    add(std::make_unique<CourseMaxPeriodsPerDayCost>());
    add(std::make_unique<BlockAdjacencyCost>());
    add(std::make_unique<TeacherMaxPeriodsCost>(TeacherMaxPeriodsCost::kDefaultWeight, maskHub));
    add(std::make_unique<TeacherMaxConsecutivePeriodsCost>(
            TeacherMaxConsecutivePeriodsCost::kDefaultWeight, maskHub));
    add(std::make_unique<TeacherGapCost>(TeacherGapCost::kDefaultWeight, maskHub));
    add(std::make_unique<TeacherWorkdayCost>(TeacherWorkdayCost::kDefaultWeight, maskHub));
    add(std::make_unique<TeacherFreeDayCost>(TeacherFreeDayCost::kDefaultWeight, maskHub));
    add(std::make_unique<ClassGapCost>());
    add(std::make_unique<AvoidedPeriodCost>());
    add(std::make_unique<NonPreferredPeriodCost>());
    add(std::make_unique<FixedSlotsCost>());
    add(std::make_unique<ParallelGroupsCost>());
    return set;
}

}  // namespace detail

inline ConstraintAssembly defaultConstraints(std::shared_ptr<TeacherMaskHub> maskHub = nullptr) {
    return std::move(detail::buildDefaultSet(std::move(maskHub)).assembly);
}

// The default set's registry names in catalog order: what list_constraints()
// and constraint_info() follow (canonical_plugin_test checks it names
// exactly defaultConstraints()).
inline std::vector<std::string> constraintOrder() { return detail::buildDefaultSet(nullptr).order; }

// Pours the default set straight into a calculator.
inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace st::canon

#endif  // ST_CANONICAL_CONSTRAINTS_HPP
