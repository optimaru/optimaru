// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MODEL_DERIVED_STATE_HPP
#define OPTIMARU_MODEL_DERIVED_STATE_HPP

namespace optimaru {

// ---------------------------------------------------------------------------
// DerivedState
// ---------------------------------------------------------------------------
// Values stored on the solution that are computed from the decision variables
// (e.g. a visit's arrival time, computed from its position in a route). The
// Editor updates them around every write, so the objective can simply read
// them:
//
//   objective.before_*(...)     // retract: the OLD derived values are in place
//   <decision variable written>
//   derived.after_*(...)        // recompute what the write affected
//   objective.after_*(...)      // add back: the NEW derived values are in place
//
// rebuild() recomputes every derived value from scratch; it runs before the
// objective's rebuild (at solve start, after restoring the best solution,
// and on every ScoreChecks::Full verification, so a buggy DerivedState
// surfaces as ScoreMismatchError).
//
// Register with Solver::add_derived_state(); several run in registration
// order. Derived values of unassigned elements are unspecified: an objective
// must not read them.
template <typename Solution>
class DerivedState {
public:
    virtual ~DerivedState() = default;

    virtual void rebuild(Solution& solution) = 0;

    // assignment shape: entity_index's genuine variable just changed.
    virtual void after_assign(Solution& /*solution*/, int /*entity_index*/) {}

    // sequence shape: list_index's contents just changed.
    virtual void after_list_edit(Solution& /*solution*/, int /*list_index*/) {}
};

}  // namespace optimaru

#endif  // OPTIMARU_MODEL_DERIVED_STATE_HPP
