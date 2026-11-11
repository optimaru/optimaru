#ifndef QAP_CONSTRAINTS_COMMON_HPP
#define QAP_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"

namespace qap {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The five quadratic-assignment constraints, one small component each on the
// engine's DecomposedObjective — an assembly path NEXT TO the
// monolithic QuadraticAssignmentIncrementalScoreCalculator, which stays the
// tuned default fast path.
//
// Unit choice: the assignment shape's unit is the VALUE, i.e. one LOCATION;
// its members are the facilities standing there (exactly one in a bijection).
// The three side constraints are per location, so they are UnitConstraints
// and the exponent shapes each location's cost:
//   pinnedAssignmentsCost      1 when a fixed location does not hold its
//                              facility
//   forbiddenAssignmentsCost   1 per facility standing where it is forbidden
//   locationCapacityCost       demand above the location's capacity
// The two objectives are one number for the whole assignment, so they are
// GlobalConstraints and the exponent shapes their total:
//   assignmentCost             summed direct facility-location cost
//   flowDistanceCost           summed flow * distance over facility pairs
// Both globals take the engine's member-change fast path (one notification
// per facility joining / leaving a location, no unit diff).
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score. The objectives are not validated >= 0 (flows,
// distances and direct costs are plain integers), so they may be negative: a
// negative total is scored as a bonus, exactly like the Python reference
// scorer. On data with a negative entry such an objective cannot move to hard
// (raw_cost_can_be_negative; the calculator refuses it at reset), since a
// negative hard total would cancel a violation of the side constraints.
//
// Unassigned facilities (location == -1, only in a partial state a caller
// wrote) contribute nothing, except that a fixed location whose facility is
// not there counts 1 (an empty fixed location is a broken pin).
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overloads and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedObjective<QuadraticAssignmentProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<QuadraticAssignmentProblem>;
using QapUnitConstraint = optimaru::UnitConstraint<QuadraticAssignmentProblem>;

namespace detail {

inline void requirePrepared(const QuadraticAssignmentProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error("qap constraints require ScoreCalculator::prepare(problem) first");
    }
}

// Whether a matrix of the facts holds a negative entry: what makes an
// objective's total possibly negative (raw_cost_can_be_negative). Read from the
// facts themselves, so it needs no prepare().
inline bool hasNegative(const std::vector<std::vector<std::int64_t>>& matrix) {
    for (const auto& row : matrix) {
        for (std::int64_t value : row) {
            if (value < 0) return true;
        }
    }
    return false;
}

inline bool hasNegative(const std::optional<std::vector<std::vector<std::int64_t>>>& matrix) {
    return matrix.has_value() && hasNegative(*matrix);
}

}  // namespace detail

// A global constraint kept from member joins and leaves alone.
using QapMemberChangeGlobalConstraint =
        optimaru::MemberChangeGlobalConstraint<QuadraticAssignmentProblem>;

}  // namespace qap

#endif  // QAP_CONSTRAINTS_COMMON_HPP
