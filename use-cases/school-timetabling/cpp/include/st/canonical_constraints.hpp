#ifndef ST_CANONICAL_CONSTRAINTS_HPP
#define ST_CANONICAL_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Canonical constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, grouped by what they share:
//   slot/     the per-slot family. They stay together because the calculator
//             compiles every present member into ONE SlotDeltaGroup evaluate
//             per member change — that structural fast path is why the plugin
//             path can stand next to the monolithic calculator at all.
//   global/   the rules whose state spans slots (per course, block group).
//   teacher/  the per-(teacher, day) mask family, sharing ONE TeacherMaskHub
//             projection per assembly instead of five private copies.
// common.hpp holds what all of them need; class_gap.hpp keeps its own
// class-keyed mask instance.
//
// Including this header still gives the whole set, so nothing that used it
// has to change. Adding a component is a file plus a line here (and a line in
// addCanonicalConstraints below when it belongs to the default assembly).

#include "st/constraints/class_gap.hpp"
#include "st/constraints/common.hpp"
#include "st/constraints/global/block_adjacency.hpp"
#include "st/constraints/global/course_daily_limit.hpp"
#include "st/constraints/global/global_constraint.hpp"
#include "st/constraints/global/unassigned_lesson.hpp"
#include "st/constraints/slot/avoided_period.hpp"
#include "st/constraints/slot/class_conflict.hpp"
#include "st/constraints/slot/non_preferred_period.hpp"
#include "st/constraints/slot/room_capacity.hpp"
#include "st/constraints/slot/slot_constraint.hpp"
#include "st/constraints/slot/slot_delta_group.hpp"
#include "st/constraints/slot/slot_not_allowed.hpp"
#include "st/constraints/teacher/teacher_consecutive_limit.hpp"
#include "st/constraints/teacher/teacher_free_day.hpp"
#include "st/constraints/teacher/teacher_gap.hpp"
#include "st/constraints/teacher/teacher_load_limit.hpp"
#include "st/constraints/teacher/teacher_mask_hub.hpp"
#include "st/constraints/teacher/teacher_workday.hpp"

namespace st::canon
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full canonical set: H1~H10 hard at weight 1 plus S1~S6 soft weighted
// by `weights` (default: the CP-SAT ObjectiveWeights defaults). With
// `weights == table.weights` this assembly scores bit-for-bit like
// ScoreCalculator::computeBreakdown and the monolithic incremental
// calculator — the components themselves never read Timetable::weights.
//
// `defaultConstraints(weights)` hands the set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints(weights)
//         .weight("S1 teacher gap", 40)
//         .drop("S2 teacher workday")
//         .into(calc);
inline ConstraintAssembly defaultConstraints(const ObjectiveWeights &weights
                                             = ObjectiveWeights{})
{
    ConstraintAssembly assembly;
    // One shared projection for the whole H9/H10/S1~S3 mask family (the
    // members are all registered with the same calculator, the hub's sharing
    // contract).
    auto maskHub = std::make_shared<TeacherMaskHub>();
    assembly.emplace<UnassignedLesson>();
    assembly.emplace<ClassConflict>();
    assembly.emplace<TeacherConflict>();
    assembly.emplace<SlotNotAllowed>();
    assembly.emplace<RoomCapacity>();
    assembly.emplace<CourseDailyLimit>();
    assembly.emplace<BlockAdjacency>();
    assembly.emplace<TeacherLoadLimit>(1.0, maskHub);
    assembly.emplace<TeacherConsecutiveLimit>(1.0, maskHub);
    assembly.emplace<TeacherGap>(static_cast<double>(weights.teacherGap),
                                 maskHub);
    assembly.emplace<TeacherWorkday>(
        static_cast<double>(weights.teacherWorkday), maskHub);
    assembly.emplace<TeacherFreeDay>(
        static_cast<double>(weights.teacherFreeDay), maskHub);
    assembly.emplace<ClassGap>(static_cast<double>(weights.classGap));
    assembly.emplace<AvoidedPeriod>(static_cast<double>(weights.avoidedPeriod));
    assembly.emplace<NonPreferredPeriod>(
        static_cast<double>(weights.nonPreferredPeriod));
    return assembly;
}

// The two names the repo uses for pouring the default set straight into a
// calculator: addDefaultConstraints is the uniform one every other use case
// exposes, addCanonicalConstraints the name this domain had first.
inline void addDefaultConstraints(DecomposedCalculator &calculator,
                                  const ObjectiveWeights &weights
                                  = ObjectiveWeights{})
{
    defaultConstraints(weights).into(calculator);
}

inline void addCanonicalConstraints(DecomposedCalculator &calculator,
                                    const ObjectiveWeights &weights
                                    = ObjectiveWeights{})
{
    addDefaultConstraints(calculator, weights);
}

}  // namespace st::canon

#endif  // ST_CANONICAL_CONSTRAINTS_HPP
