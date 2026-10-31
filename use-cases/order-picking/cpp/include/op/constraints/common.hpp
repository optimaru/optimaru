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
// The four constraints of the reference order-picking
// OrderPickingConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO the
// monolithic OrderPickingIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one trolley; members are its pick tasks IN PICKING ORDER. This domain
// decomposes perfectly: all four rules are per-trolley sums, so every
// component is a UnitConstraint and there is no GlobalConstraint at all —
// including orderSplitByTrolleyCost, because "how many trolleys carry
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
//   defaultConstraints()                         // the whole default set
//       .weight("orderSplitByTrolleyCost", 100)  // ... then only what differs
//       .soft("trolleyCapacityCost")
//       .drop("distanceFromLastPickTaskToPathOriginCost")
//       .into(calc);
//   solver.solve(solution, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent`, or by name through ConstraintAssembly. The monolithic
// calculators read the same kDefaultWeight constants, which is why the
// default assembly scores bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("trolleyCapacityCost"); Python sees them through pythonName()
// ("trolley_capacity_cost").
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation (orderSplitByTrolleyCost is the documented
// exception: it counts every (order, trolley) pair, so it is 0 only without
// assigned picks); the calculator negates into the score. An assembly of all
// four components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<OrderPickingSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<OrderPickingSolution>;
using OpUnitConstraint = optimaru::UnitConstraint<OrderPickingSolution>;
using OpGlobalConstraint = optimaru::GlobalConstraint<OrderPickingSolution>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (ScoreCalculator's ConstraintTotals and the incremental
// calculator), which are the default assembly computed in int64 like the
// Java long arithmetic. Fails to compile if a default weight stops being
// integral or if a component leaves the level the monolith sums it into.
template <typename Component, optimaru::Level kLevel>
constexpr std::int64_t monolithicWeight()
{
    static_assert(Component::kDefaultLevel == kLevel,
                  "the monolithic calculators sum this constraint into "
                  "another level");
    constexpr auto weight
        = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(weight == Component::kDefaultWeight,
                  "the monolithic calculators assume integral default "
                  "weights");
    return weight;
}
template <typename Component>
constexpr std::int64_t kHardWeight
    = monolithicWeight<Component, optimaru::Level::Hard>();
template <typename Component>
constexpr std::int64_t kSoftWeight
    = monolithicWeight<Component, optimaru::Level::Soft>();

}  // namespace op

#endif  // OP_CONSTRAINTS_COMMON_HPP
