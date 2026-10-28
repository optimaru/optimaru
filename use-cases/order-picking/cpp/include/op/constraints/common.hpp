#ifndef OP_CONSTRAINTS_COMMON_HPP
#define OP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <vector>

#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace op
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four constraints of the Timefold order-picking
// OrderPickingConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO the
// monolithic OrderPickingIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one trolley; members are its pick tasks IN PICKING ORDER. This domain
// decomposes perfectly: all four rules are per-trolley sums, so every
// component is a UnitConstraint and there is no GlobalConstraint at all —
// including minimizeOrderSplitByTrolley, because "how many trolleys carry
// this order" summed over orders is the same number as "how many distinct
// orders sit on this trolley" summed over trolleys.
//
// A component never reads the PickTask shadow variables (trolleyIndex,
// previousPickTask, nextPickTask): it walks the unit's own member order,
// which is what makes each cost a pure function of (solution, unit, members)
// as the engine's contract requires. The LIST path re-costs a touched list
// whole, so no delta contract is needed.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(solution, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The "Hard:"/"Soft:" level and the weight of each component are DEFAULTS —
// assembly-time metadata, not part of the cost. Override them per instance
// via the mutable `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all four
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<OrderPickingSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<OrderPickingSolution>;
using OpUnitConstraint = optimaru::UnitConstraint<OrderPickingSolution>;
using OpGlobalConstraint = optimaru::GlobalConstraint<OrderPickingSolution>;

}  // namespace op

#endif  // OP_CONSTRAINTS_COMMON_HPP
