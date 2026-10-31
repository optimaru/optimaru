#ifndef MS_CONSTRAINTS_HPP
#define MS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the three shared bases — per-assignment
// unit, cross-cell pair, attendance arc) and carries the pattern's
// documentation.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "ms/constraints/assign_larger_rooms_first.hpp"
#include "ms/constraints/avoid_overtime.hpp"
#include "ms/constraints/common.hpp"
#include "ms/constraints/do_meetings_as_soon_as_possible.hpp"
#include "ms/constraints/one_break_between_consecutive_meetings.hpp"
#include "ms/constraints/overlapping_meetings.hpp"
#include "ms/constraints/preferred_attendance_conflict.hpp"
#include "ms/constraints/required_and_preferred_attendance_conflict.hpp"
#include "ms/constraints/required_attendance_conflict.hpp"
#include "ms/constraints/required_room_capacity.hpp"
#include "ms/constraints/room_conflict.hpp"
#include "ms/constraints/room_stability.hpp"
#include "ms/constraints/start_and_end_on_same_day.hpp"

namespace ms
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's level weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("assignLargerRoomsFirst", 2)
//         .drop("avoidOvertime")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<RoomConflict>();
    assembly.emplace<AvoidOvertime>();
    assembly.emplace<RequiredAttendanceConflict>();
    assembly.emplace<RequiredRoomCapacity>();
    assembly.emplace<StartAndEndOnSameDay>();
    assembly.emplace<RequiredAndPreferredAttendanceConflict>();
    assembly.emplace<PreferredAttendanceConflict>();
    assembly.emplace<DoMeetingsAsSoonAsPossible>();
    assembly.emplace<OneBreakBetweenConsecutiveMeetings>();
    assembly.emplace<OverlappingMeetings>();
    assembly.emplace<AssignLargerRoomsFirst>();
    assembly.emplace<RoomStability>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ms

#endif  // MS_CONSTRAINTS_HPP
