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
// One small component per constraint on the engine's DecomposedScoreCalculator
// (30 in all; eight of them form the default assembly). A unit is an
// employee; members are the shift indices currently assigned to that
// employee. The eight defaults reuse the exact ScoreCalculator building
// blocks, so the default assembly reproduces the monolithic score
// bit-for-bit.
//
// Every unit constraint also implements the engine's incremental contract.
// Compatible assignment constraints share one structural delta-group key, so
// the calculator automatically compiles any subset of them into one marginal
// evaluator while unrelated constraints keep their normal deltaCost path.
//
//   DecomposedCalculator calc;
//   calc.emplace<RequiredSkillCost>();                 // pick constraints...
//   calc.emplace<EmployeeUnavailabilityCost>(2.0);     // ...tune weights
//   calc.emplace<MaxConsecutiveDaysCost>(MaxConsecutiveDaysCost::kDefaultWeight, 5);
//   solver.solve(schedule, calc);
//
// The pattern every component follows:
//   - the struct is <Subject>Cost, in constraints/<subject>_cost.hpp, and its
//     registry name is the struct name with a lower-case first letter
//     ("requiredSkillCost"); Python sees it as required_skill_cost
//     (optimaru::python::pythonName). The name never carries parameters.
//   - the defaults are declared ONCE, as `static constexpr double
//     kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
//     and the constructor takes the weight first, then its domain parameters
//     (each with a neutral default, so catalogConstraints() can build it).
//   - a parametrized component sets `label` (display text of the instance,
//     e.g. "Day pattern[A,OFF,B]"): one calculator may hold several instances
//     of one registry name, and the label tells them apart in
//     constraintTotals(). It is never a lookup key.
//   - a reward (raw cost <= 0: DesiredDayCost, DesiredShiftCost) sets
//     `reward = true; levelFlippable = false;` in its constructor.
//   - the comment above each struct states the default level and weight, what
//     one unit of raw cost is, and that 0 means no violation.
// The level is assembly-time metadata, not part of the cost: re-state it per
// instance with `level` / setLevel() (ConstraintAssembly hard()/soft(),
// Python level). Same for `weight` and `exponent`.
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
// grouping keys off the concrete type (a ScopedUnit is not RequiredSkillCost
// etc.), and inner constraints that use a shared context (e.g. DayPatternCost)
// fall back to their own local context when none is bound (scoped_unit.hpp).

}  // namespace es

#endif  // ES_CONSTRAINTS_COMMON_HPP
