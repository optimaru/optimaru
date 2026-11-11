#ifndef BP_CONSTRAINTS_COMMON_HPP
#define BP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "bp/domain.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace bp {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The four bin-packing constraints, one small component each on the engine's
// DecomposedObjective — an assembly path NEXT TO the monolithic
// BinPackingIncrementalScoreCalculator, which stays the tuned default fast
// path.
//
// Unit choice: the assignment shape's unit is the VALUE, i.e. one bin;
// members are the items currently in it (order irrelevant). Capacity, colour
// and conflict violations all decompose per bin, so they are UnitConstraints
// (one cost per bin 0..n-1, an empty bin costs 0). usedBinsCost is a
// GlobalConstraint instead: its exponent shapes the bin COUNT (count ** e),
// which per-bin units (sum of 1 ** e) could not express.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overloads, the bindings and the monolithic path call it).

using DecomposedCalculator = optimaru::DecomposedObjective<BinPackingProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<BinPackingProblem>;
using BpUnitConstraint = optimaru::UnitConstraint<BinPackingProblem>;
using BpGlobalConstraint = optimaru::GlobalConstraint<BinPackingProblem>;

namespace detail {

inline void requirePrepared(const BinPackingProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error(
                "bp constraints require "
                "ScoreCalculator::prepare(problem) first");
    }
}

}  // namespace detail

}  // namespace bp

#endif  // BP_CONSTRAINTS_COMMON_HPP
