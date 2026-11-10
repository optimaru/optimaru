#ifndef FL_CONSTRAINTS_COMMON_HPP
#define FL_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The three facility-location constraints, one small component each on
// the engine's DecomposedObjective — an optional assembly path NEXT TO
// the monolithic FacilityLocationIncrementalScoreCalculator, which stays the
// tuned default fast path.
//
// Unit choice: the assignment shape's unit is the VALUE, i.e. one facility;
// members are the consumers currently assigned to it. Every constraint is a
// function of one facility and its consumers, so all three decompose per
// unit and none of them needs a GlobalConstraint. "Facility serves at least
// one consumer" — the condition gating both the capacity and the setup-cost
// term — is simply `!members.empty()` here.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                     // the whole default set
//       .weight("facilitySetupCost", 4)      // ... then only what differs
//       .soft("facilityCapacityCost")
//       .drop("distanceFromFacilityCost")
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
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent`, or by name through ConstraintAssembly. The canonical problem
// carries no weights: the monolithic calculators read the
// same kDefaultWeight constants, which is why the default assembly scores
// bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("facilityCapacityCost"); Python sees them through python_name()
// ("facility_capacity_cost").
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedObjective<FacilityLocationProblem>;
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
