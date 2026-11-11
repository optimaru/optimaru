#ifndef FL_CONSTRAINTS_COMMON_HPP
#define FL_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace fl {

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
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedObjective<FacilityLocationProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<FacilityLocationProblem>;
using FlUnitConstraint = optimaru::UnitConstraint<FacilityLocationProblem>;
using FlGlobalConstraint = optimaru::GlobalConstraint<FacilityLocationProblem>;

namespace detail {

inline void requirePrepared(const FacilityLocationProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error(
                "fl constraints require "
                "ScoreCalculator::prepare(problem) first");
    }
}

}  // namespace detail

}  // namespace fl

#endif  // FL_CONSTRAINTS_COMMON_HPP
