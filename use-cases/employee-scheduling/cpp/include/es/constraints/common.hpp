#ifndef ES_CONSTRAINTS_COMMON_HPP
#define ES_CONSTRAINTS_COMMON_HPP
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "es/domain.hpp"
#include "es/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace es {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The eight default constraints, split into one small component each on the
// engine's DecomposedScoreCalculator. A unit is an employee; members are the shift
// indices currently assigned to that employee. Each component reuses the
// exact ScoreCalculator building blocks, so any assembly of all eight at
// weight 1 reproduces the monolithic score bit-for-bit.
//
// Every unit constraint also implements the engine's incremental contract.
// Compatible assignment constraints share one structural delta-group key, so
// the calculator automatically compiles any subset of them into one marginal
// evaluator while unrelated constraints keep their normal deltaCost path.
//
//   DecomposedCalculator calc;
//   calc.emplace<RequiredSkill>();          // pick constraints...
//   calc.emplace<UnavailableEmployee>(2.0); // ...tune weights
//   solver.solve(schedule, calc);
//
// The "Hard:"/"Soft:" tag on each constraint is its DEFAULT level. The level
// is assembly-time metadata, not part of the cost: override it per instance via
// the mutable
// `isHard` (Python: add(..., hard=True/False) or RequiredSkill(hard=False)).
// Same for `weight` and `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, negative = reward; the calculator negates into the score.

using DecomposedCalculator = optimaru::DecomposedScoreCalculator<EmployeeSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<EmployeeSchedule>;
using EsUnitConstraint = optimaru::UnitConstraint<EmployeeSchedule>;
using EsGlobalConstraint = optimaru::GlobalConstraint<EmployeeSchedule>;

// Rule scope: restrict any unit constraint to a subset of workers and/or
// shifts. The caller resolves worker/shift ids or tags to engine indices before
// solving; this wrapper honours them. Orthogonal to each constraint's logic —
// no per-constraint code changes needed.
//
//   worker scope: the constraint is skipped entirely for out-of-scope workers
//                 (their cost contribution is 0).
//   shift scope:  only in-scope shifts are counted (members are filtered
//                 before the inner cost runs).
//
// Empty index list => "all" for that dimension. Indices are resolved against
// the solution order by the caller; onReset expands them to constant-time masks.
//
// The inner constraint keeps its own cost/deltaCost. Scoped constraints opt
// out of the calculator's structural delta-grouping and shared-context fast
// paths (those virtuals stay at their nullptr defaults here), which is correct:
// grouping keys off the concrete type (a ScopedUnit is not RequiredSkill etc.),
// and inner constraints that use a shared context (e.g. DayPattern) fall back
// to their own local context when none is bound.

}  // namespace es

#endif  // ES_CONSTRAINTS_COMMON_HPP
