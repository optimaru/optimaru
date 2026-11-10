#ifndef BP_CONSTRAINTS_COMMON_HPP
#define BP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "bp/domain.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace bp
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The four bin-packing constraints, one small component each on the engine's
// DecomposedObjective — an assembly path NEXT TO the monolithic
// BinPackingIncrementalScoreCalculator, which stays the tuned default fast
// path.
//
// Unit choice: the BASIC descriptor's unit is the VALUE, i.e. one bin;
// members are the items currently in it (order irrelevant). Capacity, colour
// and conflict violations all decompose per bin, so they are UnitConstraints
// (one cost per bin 0..n-1, an empty bin costs 0). usedBinsCost is a
// GlobalConstraint instead: its exponent shapes the bin COUNT (count ** e),
// which per-bin units (sum of 1 ** e) could not express.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                     // the whole default set
//       .weight("usedBinsCost", 4)           // ... then only what differs
//       .soft("binCapacityCost")
//       .drop("colorConflictCost")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the validated setters
// (set_weight / set_level / set_exponent, or the mutable `level`), or by name
// through ConstraintAssembly. The canonical problem carries no weights:
// the monolithic calculators read the same
// kDefaultWeight constants, which is why the default assembly scores
// bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("binCapacityCost"); Python sees them through python_name()
// ("bin_capacity_cost").
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overloads, the bindings and the monolithic path call it).

using DecomposedCalculator = optimaru::DecomposedObjective<BinPackingProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<BinPackingProblem>;
using BpUnitConstraint = optimaru::UnitConstraint<BinPackingProblem>;
using BpGlobalConstraint = optimaru::GlobalConstraint<BinPackingProblem>;

namespace detail
{

inline void requirePrepared(const BinPackingProblem &problem)
{
    if (!problem.prepared)
    {
        throw std::logic_error("bp constraints require "
                               "ScoreCalculator::prepare(problem) first");
    }
}

}  // namespace detail

}  // namespace bp

#endif  // BP_CONSTRAINTS_COMMON_HPP
