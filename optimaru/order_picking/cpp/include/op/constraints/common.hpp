#ifndef OP_CONSTRAINTS_COMMON_HPP
#define OP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <vector>

#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace op {

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
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score. orderSplitByTrolleyCost is the one cost that is not
// 0 without a violation: it counts every (order, trolley) pair, so it is 0
// only without assigned picks.

using DecomposedCalculator = optimaru::DecomposedObjective<OrderPickingSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<OrderPickingSolution>;
using OpUnitConstraint = optimaru::UnitConstraint<OrderPickingSolution>;
using OpGlobalConstraint = optimaru::GlobalConstraint<OrderPickingSolution>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (ScoreCalculator's ConstraintTotals and the incremental
// calculator), which are the default assembly computed in int64. Fails to
// compile if a default weight stops being
// integral or if a component leaves the level the monolith sums it into.
template <typename Component, optimaru::Level kLevel>
constexpr std::int64_t monolithicWeight() {
    static_assert(Component::kDefaultLevel == kLevel,
                  "the monolithic calculators sum this constraint into "
                  "another level");
    constexpr auto weight = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(weight == Component::kDefaultWeight,
                  "the monolithic calculators assume integral default "
                  "weights");
    return weight;
}
template <typename Component>
constexpr std::int64_t kHardWeight = monolithicWeight<Component, optimaru::Level::Hard>();
template <typename Component>
constexpr std::int64_t kSoftWeight = monolithicWeight<Component, optimaru::Level::Soft>();

}  // namespace op

#endif  // OP_CONSTRAINTS_COMMON_HPP
