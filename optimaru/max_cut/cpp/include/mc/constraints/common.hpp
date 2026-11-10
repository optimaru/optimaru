#ifndef MC_CONSTRAINTS_COMMON_HPP
#define MC_CONSTRAINTS_COMMON_HPP

#include <stdexcept>
#include <vector>

#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The six max-cut constraints, one small component each on the engine's
// DecomposedObjective — the assembly path NEXT TO the monolithic
// MaxCutIncrementalScoreCalculator, which stays the default fast path.
//
// Unit choice: the assignment shape's units are the two VALUES — unit 0 holds
// the side-0 vertices, unit 1 the side-1 vertices (an unassigned vertex, -1,
// is in neither). Four constraints decompose per side and are
// UnitConstraints (constraint_costs() reports [side 0, side 1]):
//   fixedPartitionCost           fixed vertices of this side that belong on
//                                the other one              (per-member delta)
//   samePartitionPairsCost       same-side pairs whose first endpoint is on
//                                this side and whose second is not
//                                                            (pair delta)
//   differentPartitionPairsCost  different-side pairs with both endpoints on
//                                this side                   (pair delta)
//   partitionSizeBoundsCost      max(0, min - n) + max(0, n - max) for this
//                                side's size n               (O(1) re-cost)
// and two are statistics of the whole partition, GlobalConstraints fed by
// on_member_changed (supports_member_change, O(1) / O(degree) per join/leave):
//   partitionImbalanceCost       |n0 - n1|
//   cutWeightCost                -(weight of the edges between the sides)
//
// Consequences of the unit choice (the engine convention, see the package
// README): the exponent shapes each side's cost, not each 0/1 pair or vertex
// term, and a hard cost is rounded per side. At exponent 1 and integral
// weights both are bit-identical to the Python reference's per-term scoring.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                          // the whole default set
//       .weight("partitionImbalanceCost", 1)      // ... then only what differs
//       .soft("partitionSizeBoundsCost")
//       .drop("fixedPartitionCost")
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
// ("fixedPartitionCost"); Python sees them through python_name()
// ("fixed_partition_cost").
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty, 0 = no violation; cutWeightCost is a reward (raw <= 0, never
// hard). The calculator negates into the score.
//
// The pair deltas read the partner's side from the working solution. That is
// exact because the engine notifies assignment changes one entity at a time
// (before / set / after): at every delta_cost() call each OTHER vertex's side
// equals its unit membership. ScoreChecks::Full solves (swaps and ruin-and-recreate
// included) and the random-walk tests check it.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (both solve() overloads and the monolithic path call it).

using DecomposedCalculator = optimaru::DecomposedObjective<MaxCutProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<MaxCutProblem>;
using McUnitConstraint = optimaru::UnitConstraint<MaxCutProblem>;
using McGlobalConstraint = optimaru::GlobalConstraint<MaxCutProblem>;

namespace detail {

inline void requirePrepared(const MaxCutProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error(
                "mc constraints require "
                "ScoreCalculator::prepare(problem) first");
    }
}

// Membership flags of one unit's members (a unit cost is a pure function of
// (unit, members); cost() is only called at reset and in tests, so the O(V)
// scratch is cheap).
inline std::vector<char> memberFlags(const MaxCutProblem& problem,
                                     const std::vector<int>& members) {
    std::vector<char> in(problem.vertexIds.size(), 0);
    for (int v : members) in[v] = 1;
    return in;
}

// The common shape of the two globals: driven by joins / leaves.
struct MemberChangeConstraint : McGlobalConstraint {
    using McGlobalConstraint::McGlobalConstraint;

    bool supports_member_change() const final { return true; }

    // Never called (supports_member_change): every change arrives as a
    // join/leave through on_member_changed.
    void on_unit_changed(const MaxCutProblem& /*problem*/, int /*unit*/,
                         const std::vector<int>& /*members*/) final {}
};

}  // namespace detail

}  // namespace mc

#endif  // MC_CONSTRAINTS_COMMON_HPP
