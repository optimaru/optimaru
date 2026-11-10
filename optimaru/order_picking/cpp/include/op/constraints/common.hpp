#ifndef OP_CONSTRAINTS_COMMON_HPP
#define OP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <vector>

#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace op
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four order-picking constraints, one small component each on the
// engine's DecomposedObjective — an optional assembly path NEXT TO the
// monolithic OrderPickingIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the sequence shape the calculator's unit is one list,
// i.e. one trolley; members are its pick tasks IN PICKING ORDER. This domain
// decomposes perfectly: all four rules are per-trolley sums, so every
// component is a UnitConstraint and there is no GlobalConstraint at all —
// including orderSplitByTrolleyCost, because "how many trolleys carry
// this order" summed over orders is the same number as "how many distinct
// orders sit on this trolley" summed over trolleys.
//
// A component never reads the PickTask derived fields (trolleyIndex,
// previousPickTask, nextPickTask): it walks the unit's own member order,
// which is what makes each cost a pure function of (solution, unit, members)
// as the engine's contract requires. The sequence path re-costs a touched list
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
// ("trolleyCapacityCost"); Python sees them through python_name()
// ("trolley_capacity_cost").
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty, 0 = no violation (orderSplitByTrolleyCost is the documented
// exception: it counts every (order, trolley) pair, so it is 0 only without
// assigned picks); the calculator negates into the score. An assembly of all
// four components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::score.

using DecomposedCalculator
    = optimaru::DecomposedObjective<OrderPickingSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<OrderPickingSolution>;
using OpUnitConstraint = optimaru::UnitConstraint<OrderPickingSolution>;
using OpGlobalConstraint = optimaru::GlobalConstraint<OrderPickingSolution>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (ScoreCalculator's ConstraintTotals and the incremental
// calculator), which are the default assembly computed in int64. Fails to
// compile if a default weight stops being
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
