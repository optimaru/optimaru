#ifndef TS_CONSTRAINTS_COMMON_HPP
#define TS_CONSTRAINTS_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"

namespace ts {

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four constraints of this use case, split into one small component each on
// the engine's DecomposedObjective — an optional assembly path NEXT TO
// the monolithic TournamentSchedulingIncrementalScoreCalculator, which stays
// the tuned default fast path.
//
// Unit choice: with the assignment shape the calculator's unit is the VALUE,
// i.e. one team; members are the assignments currently given to it.
// Unassigned assignments (team == -1) belong to no unit and so contribute
// nothing, exactly like in the monolithic path.
//
// The two hard rules are per-team sums and run as UnitConstraints. Both
// load-balance rules are statistics ACROSS units — an unfairness over the
// per-team loads, and one over the per-team-PAIR confrontation counts — so
// they run as GlobalConstraints on the on_member_changed fast path, maintaining
// exactly the aggregates the monolithic calculator keeps.
//
// Names and defaults: every component is one `<Subject>Cost` struct in
// constraints/<subject>_cost.hpp whose registry name is the struct name with a
// lower-case first letter ("teamConflictCost"; Python sees it as
// team_conflict_cost). Its default level and weight are declared ONCE, as
// `kDefaultLevel` / `kDefaultWeight`; the constructor takes the weight first
// and defaults it to kDefaultWeight. constraint_info() and the CP-SAT
// reference read them from there, and the monolithic calculators read the
// same constants (score_calculator.cpp / incremental_calculator.cpp).
//
//   teamConflictCost                 hard,   1  same-day assignment pairs
//   teamUnavailabilityCost           hard,   1  violated unavailability facts
//   assignmentCountImbalanceCost     medium, 1  unfairness of per-team counts
//   confrontationCountImbalanceCost  soft,   1  unfairness of per-pair counts
//
// Score levels: this use case scores on hard / medium / soft levels. This
// domain keeps the engine's Score and declares how its medium level
// folds into soft ONCE, in domain.hpp (kMediumFold = into_soft(kMediumScale =
// 1e6), registered through optimaru::MediumFoldOf<TournamentSchedule>). The
// medium component is `Level::Medium` at weight 1 in medium units; the
// decomposed calculator keeps a separate medium accumulator and folds it with
// that constant. Both unfairness values go through
// ScoreCalculator::unfairnessFromAggregates, so an assembly of all four
// reproduces the monolithic double bit-for-bit.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md.
//
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance through the
// assembly's knob setters (`weight` / `level` / `exponent` / `offset` /
// `clip`; `hard` / `soft` are level(Hard) / level(Soft)) or the validated
// `set_weight` / `set_level` / `set_exponent` / `set_offset` / `set_clip` of the
// component (`component->set_level(optimaru::Level::Soft)`). Any component may
// take any of the three levels: the medium component moved to hard costs 1
// hard per raw unit (rounded), moved to soft 1 soft per raw unit (outside
// the fold), a hard or soft one moved to medium 1 medium unit; an explicit
// weight is in units of the level in effect. A name may be held several
// times (ConstraintAssembly::add_instances), each instance with its own knobs.

using DecomposedCalculator = optimaru::DecomposedObjective<TournamentSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<TournamentSchedule>;
using TsUnitConstraint = optimaru::UnitConstraint<TournamentSchedule>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (score_calculator.cpp / incremental_calculator.cpp), which are
// the default assembly computed by hand. Fails to compile if a default weight
// stops being integral or if a component leaves the level the monolith sums
// it into.
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
constexpr std::int64_t kMediumWeight = monolithicWeight<Component, optimaru::Level::Medium>();
template <typename Component>
constexpr std::int64_t kSoftWeight = monolithicWeight<Component, optimaru::Level::Soft>();

namespace detail {

inline int dayOf(const TournamentSchedule& schedule, int assignmentIndex) {
    return schedule.teamAssignments[assignmentIndex].day;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Cross-unit global constraints
// ---------------------------------------------------------------------------
// Shared base: the aggregates are kept from joins and leaves alone.
using MemberChangeGlobalConstraint = optimaru::MemberChangeGlobalConstraint<TournamentSchedule>;

}  // namespace ts

#endif  // TS_CONSTRAINTS_COMMON_HPP
