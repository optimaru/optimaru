#ifndef SLS_CONSTRAINTS_HPP
#define SLS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the three shared bases — per-match unit,
// round-index pair, team-side run) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "sls/constraints/away_to_away_hop_cost.hpp"
#include "sls/constraints/away_to_end_hop_cost.hpp"
#include "sls/constraints/away_to_home_hop_cost.hpp"
#include "sls/constraints/common.hpp"
#include "sls/constraints/consecutive_away_matches_cost.hpp"
#include "sls/constraints/consecutive_home_matches_cost.hpp"
#include "sls/constraints/home_to_away_hop_cost.hpp"
#include "sls/constraints/repeat_match_on_next_day_cost.hpp"
#include "sls/constraints/start_to_away_hop_cost.hpp"
#include "sls/constraints/team_conflict_cost.hpp"
#include "sls/constraints/workday_classic_matches_cost.hpp"

namespace sls
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's weights, each component at its own
// kDefaultWeight / kDefaultLevel: four hard rules at weight 1, the five
// travel hops in plain kilometres and workdayClassicMatchesCost at x 1'000.
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator. Its info() is the domain's
// constraint catalog (the Python constraint_info()); the catalog order is
// the assembly's: the three unit constraints (startToAwayHopCost,
// awayToEndHopCost, workdayClassicMatchesCost) first, then the seven globals
// in emplace order.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("awayToAwayHopCost", 2)
//         .drop("awayToEndHopCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<TeamConflictCost>();
    assembly.emplace<ConsecutiveHomeMatchesCost>();
    assembly.emplace<ConsecutiveAwayMatchesCost>();
    assembly.emplace<RepeatMatchOnNextDayCost>();
    assembly.emplace<StartToAwayHopCost>();
    assembly.emplace<HomeToAwayHopCost>();
    assembly.emplace<AwayToAwayHopCost>();
    assembly.emplace<AwayToHomeHopCost>();
    assembly.emplace<AwayToEndHopCost>();
    assembly.emplace<WorkdayClassicMatchesCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace sls

#endif  // SLS_CONSTRAINTS_HPP
