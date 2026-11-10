#ifndef KS_CONSTRAINTS_COMMON_HPP
#define KS_CONSTRAINTS_COMMON_HPP

#include <stdexcept>
#include <vector>

#include "ks/domain.hpp"
#include "ks/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace ks {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The seven knapsack constraints, one small component each on the engine's
// DecomposedObjective — the assembly path NEXT TO the monolithic
// KnapsackIncrementalScoreCalculator, which stays the default fast path.
//
// Unit choice: the assignment shape's units are the two VALUES — unit 0 holds
// the unselected items, unit 1 the selected ones (an undecided item, -1, is in
// neither). Every constraint is a function of the selected set as a whole
// (a capacity, a count per group, a pair of items), so none decomposes per
// unit: all seven are GlobalConstraints. Each keeps its aggregate up to date
// from on_member_changed (supports_member_change) and listens to unit 1 only — an
// item joining unit 1 became selected, one leaving it stopped being selected
// — so a flip costs O(1) or O(references of the item).
//
// Consequences of "global" (the engine convention, see the package README):
// constraint_costs() reports ONE raw total per constraint, the exponent
// shapes that total, and a hard total is rounded once per constraint.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                         // the whole default set
//       .weight("resourceCapacityCost", 4)       // ... then only what differs
//       .soft("forbiddenItemsCost")
//       .drop("itemDependencyCost")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from scratch.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the validated setters
// (set_weight / set_level / set_exponent), or by name through
// ConstraintAssembly. The canonical problem carries no weights: the
// monolithic calculators read the same kDefaultWeight constants, which is why
// the default assembly scores bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("knapsackCapacityCost"); Python sees them through python_name()
// ("knapsack_capacity_cost").
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty, 0 = no violation; selectedValueCost is a reward (raw <= 0, never
// hard). The calculator negates into the score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (both solve() overloads and the monolithic path call it).

using DecomposedCalculator = optimaru::DecomposedObjective<KnapsackProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<KnapsackProblem>;
using KsGlobalConstraint = optimaru::GlobalConstraint<KnapsackProblem>;

namespace detail {

inline void requirePrepared(const KnapsackProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error(
                "ks constraints require "
                "ScoreCalculator::prepare(problem) first");
    }
}

// Selection flags of every item off the calculator's units (unit 1 = the
// selected items); undecided and unselected items are both 0.
inline std::vector<char> selectedFlags(const KnapsackProblem& problem,
                                       const std::vector<std::vector<int>>& members_per_unit) {
    std::vector<char> selected(problem.items.size(), 0);
    if (members_per_unit.size() > static_cast<std::size_t>(kSelected)) {
        for (int item : members_per_unit[kSelected]) selected[item] = 1;
    }
    return selected;
}

// The common shape of the seven components: a global constraint driven by
// joins/leaves of unit 1.
struct SelectionConstraint : KsGlobalConstraint {
    using KsGlobalConstraint::KsGlobalConstraint;

    bool supports_member_change() const final { return true; }

    void on_member_changed(const KnapsackProblem& problem, int unit,
                           const std::vector<int>& /*members*/, int item, bool joined) final {
        if (unit == kSelected) onSelectionChanged(problem, item, joined);
    }

    // Never called (supports_member_change): every change arrives as a
    // join/leave above.
    void on_unit_changed(const KnapsackProblem& /*problem*/, int /*unit*/,
                         const std::vector<int>& /*members*/) final {}

    // `item` became selected (selected == true) or stopped being selected.
    virtual void onSelectionChanged(const KnapsackProblem& problem, int item, bool selected) = 0;
};

}  // namespace detail

}  // namespace ks

#endif  // KS_CONSTRAINTS_COMMON_HPP
