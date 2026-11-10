#ifndef MS_CONSTRAINTS_HPP
#define MS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One `<Subject>Cost` component per file under constraints/<subject>_cost.hpp,
// each declaring its default level and weight once (kDefaultLevel /
// kDefaultWeight); common.hpp holds what all of them need (the calculator
// aliases and the three shared bases — per-assignment unit, cross-cell pair,
// attendance arc) and carries the pattern's documentation.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "ms/constraints/common.hpp"
#include "ms/constraints/consecutive_meetings_cost.hpp"
#include "ms/constraints/day_crossing_cost.hpp"
#include "ms/constraints/larger_room_capacity_cost.hpp"
#include "ms/constraints/meeting_end_time_cost.hpp"
#include "ms/constraints/meeting_overlap_cost.hpp"
#include "ms/constraints/overtime_cost.hpp"
#include "ms/constraints/preferred_attendance_conflict_cost.hpp"
#include "ms/constraints/required_and_preferred_attendance_conflict_cost.hpp"
#include "ms/constraints/required_attendance_conflict_cost.hpp"
#include "ms/constraints/room_capacity_cost.hpp"
#include "ms/constraints/room_change_cost.hpp"
#include "ms/constraints/room_conflict_cost.hpp"

namespace ms
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// All twelve constraints at their default weights — each component at its
// own kDefaultLevel / kDefaultWeight (the medium pair at weight 1 in medium
// units; the domain's kMediumFold puts them on the engine's soft level). This
// assembly scores bit-for-bit like ScoreCalculator::score and the
// monolithic incremental calculator. It is also the catalog behind
// constraint_info() (the Python bindings), in this order.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("largerRoomCapacityCost", 2)
//         .drop("overtimeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<RoomConflictCost>();
    assembly.emplace<OvertimeCost>();
    assembly.emplace<RequiredAttendanceConflictCost>();
    assembly.emplace<RoomCapacityCost>();
    assembly.emplace<DayCrossingCost>();
    assembly.emplace<RequiredAndPreferredAttendanceConflictCost>();
    assembly.emplace<PreferredAttendanceConflictCost>();
    assembly.emplace<MeetingEndTimeCost>();
    assembly.emplace<ConsecutiveMeetingsCost>();
    assembly.emplace<MeetingOverlapCost>();
    assembly.emplace<LargerRoomCapacityCost>();
    assembly.emplace<RoomChangeCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ms

#endif  // MS_CONSTRAINTS_HPP
