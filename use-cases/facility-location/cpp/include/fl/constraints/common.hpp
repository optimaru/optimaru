#ifndef FL_CONSTRAINTS_COMMON_HPP
#define FL_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The three constraints of the Timefold facility-location
// FacilityLocationConstraintProvider, split into one small component each on
// the engine's DecomposedScoreCalculator — an optional assembly path NEXT TO
// the monolithic FacilityLocationIncrementalScoreCalculator, which stays the
// tuned default fast path.
//
// Unit choice: the BASIC descriptor's unit is the VALUE, i.e. one facility;
// members are the consumers currently assigned to it. That is exactly the
// grouping the Java provider does (groupBy(Consumer::getFacility)), so all
// three constraints decompose per unit and none of them needs a
// GlobalConstraint. "Facility serves at least one consumer" — the condition
// gating both the capacity and the setup-cost term in Java — is simply
// `!members.empty()` here.
//
//   DecomposedCalculator calc;
//   defaultConstraints(problem)              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Or `addDefaultConstraints(calc, problem)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The "Hard:"/"Soft:" level and the weight of each component are DEFAULTS —
// assembly-time metadata, not part of the cost. Override them per instance
// via the mutable `isHard` / `weight` / `exponent`. Unlike the monolithic
// calculators the components never read FacilityLocationProblem's weight
// fields; pass the intended weights at assembly time (addDefaultConstraints
// does, and its problem overload takes them straight off the solution, which
// is where the quickstart's ConstraintWeightOverrides land).
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all three
// components with the problem's own weights therefore scores bit-for-bit
// like ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<FacilityLocationProblem>;
using ConstraintAssembly
    = optimaru::ConstraintAssembly<FacilityLocationProblem>;
using FlUnitConstraint = optimaru::UnitConstraint<FacilityLocationProblem>;
using FlGlobalConstraint = optimaru::GlobalConstraint<FacilityLocationProblem>;

namespace detail
{

inline void requirePrepared(const FacilityLocationProblem &problem)
{
    if (!problem.prepared)
    {
        throw std::logic_error("fl constraints require "
                               "ScoreCalculator::prepare(problem) first");
    }
}

}  // namespace detail

}  // namespace fl

#endif  // FL_CONSTRAINTS_COMMON_HPP
