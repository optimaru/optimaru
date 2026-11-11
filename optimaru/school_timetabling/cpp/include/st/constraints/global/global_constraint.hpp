#ifndef ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP
#define ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP

#include "st/constraints/common.hpp"

namespace st::canon {

// ---------------------------------------------------------------------------
// Cross-slot global constraints (assignment member-change fast path: the
// calculator delivers every lesson join/leave, including the inverse writes
// of a rolled-back move, so exactly reversible aggregates stay consistent)
// ---------------------------------------------------------------------------

// Shared base: the aggregates are kept from joins and leaves alone.
using MemberChangeGlobalConstraint = optimaru::MemberChangeGlobalConstraint<Timetable>;

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP
