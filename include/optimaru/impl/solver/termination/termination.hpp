#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_HPP

// ---------------------------------------------------------------------------
// Termination — umbrella header for the stop-criterion plugins.
// ---------------------------------------------------------------------------
// The plugin point and the shared per-solve state live in
// termination_criterion.hpp, one criterion per file next to it, and
// solver_termination.hpp OR-composes them from TerminationConfig.
//
// Adding a criterion: drop a file next to these, include it here, and wire it
// in SolverTermination's constructor. Including this header keeps giving you
// the whole set, so call sites never change when the set grows.

#include "optimaru/impl/solver/termination/best_score.hpp"
#include "optimaru/impl/solver/termination/solver_termination.hpp"
#include "optimaru/impl/solver/termination/step_count.hpp"
#include "optimaru/impl/solver/termination/termination_criterion.hpp"
#include "optimaru/impl/solver/termination/time_millis_spent.hpp"
#include "optimaru/impl/solver/termination/unimproved_step_count.hpp"
#include "optimaru/impl/solver/termination/unimproved_time_millis_spent.hpp"

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_HPP
