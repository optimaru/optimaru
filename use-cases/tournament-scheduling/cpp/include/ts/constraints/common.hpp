#ifndef TS_CONSTRAINTS_COMMON_HPP
#define TS_CONSTRAINTS_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"

namespace ts
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four constraints of the reference tournament-scheduling
// TournamentScheduleConstraintProvider, split into one small component each on
// the engine's DecomposedScoreCalculator — an optional assembly path NEXT TO
// the monolithic TournamentSchedulingIncrementalScoreCalculator, which stays
// the tuned default fast path.
//
// Unit choice: with the BASIC descriptor the calculator's unit is the VALUE,
// i.e. one team; members are the assignments currently given to it.
// Unassigned assignments (team == -1) belong to no unit and so contribute
// nothing, exactly like in the monolithic path.
//
// The two hard rules are per-team sums and run as UnitConstraints. Both
// load-balance rules are statistics ACROSS units — an unfairness over the
// per-team loads, and one over the per-team-PAIR confrontation counts — so
// they run as GlobalConstraints on the onMemberChanged fast path, maintaining
// exactly the aggregates the monolithic calculator keeps.
//
// Score levels: the quickstart scores with HardMediumSoftBigDecimalScore,
// which this domain folds into the engine's HardSoftScore by carrying the
// medium level at kMediumScale per unit (see domain.hpp). The medium
// component's default weight is therefore kMediumScale, and both unfairness
// values go through ScoreCalculator::unfairnessFromAggregates, so an assembly
// of all four reproduces the monolithic double bit-for-bit.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(schedule, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance via the mutable
// `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score.
//
// Naming note: the component that scores the UnavailabilityPenalty facts is
// called UnavailabilityPenaltyConstraint, because the domain fact it reads
// already owns the plain name in this namespace. Its constraint name string
// is the provider's "unavailabilityPenalty" like everywhere else.

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<TournamentSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<TournamentSchedule>;
using TsUnitConstraint = optimaru::UnitConstraint<TournamentSchedule>;
using TsGlobalConstraint = optimaru::GlobalConstraint<TournamentSchedule>;

namespace detail
{

inline int dayOf(const TournamentSchedule &schedule, int assignmentIndex)
{
    return schedule.teamAssignments[assignmentIndex].day;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Cross-unit global constraints
// ---------------------------------------------------------------------------
// Shared base: supportsMemberChange() routes everything through
// onMemberChanged; the unit-diff hook is unreachable for BASIC solutions.
struct MemberChangeGlobalConstraint : TsGlobalConstraint
{
    using TsGlobalConstraint::TsGlobalConstraint;
    bool supportsMemberChange() const override { return true; }
    void onUnitChanged(const TournamentSchedule & /*schedule*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_COMMON_HPP
