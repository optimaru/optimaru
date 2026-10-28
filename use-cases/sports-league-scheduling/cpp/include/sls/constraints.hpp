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

#include "sls/constraints/away_to_away_hop.hpp"
#include "sls/constraints/away_to_end_hop.hpp"
#include "sls/constraints/away_to_home_hop.hpp"
#include "sls/constraints/classic_matches.hpp"
#include "sls/constraints/common.hpp"
#include "sls/constraints/consecutive_away_matches.hpp"
#include "sls/constraints/consecutive_home_matches.hpp"
#include "sls/constraints/home_to_away_hop.hpp"
#include "sls/constraints/matches_on_same_day.hpp"
#include "sls/constraints/repeat_match_on_next_day.hpp"
#include "sls/constraints/start_to_away_hop.hpp"

namespace sls
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's weights: four hard rules at weight 1,
// the five travel hops in plain kilometres and classicMatches at x 1'000.
// This assembly scores bit-for-bit like ScoreCalculator::calculateScore and
// the monolithic incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("awayToAwayHop", 2)
//         .drop("awayToEndHop")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<MatchesOnSameDay>();
    assembly.emplace<ConsecutiveHomeMatches>();
    assembly.emplace<ConsecutiveAwayMatches>();
    assembly.emplace<RepeatMatchOnNextDay>();
    assembly.emplace<StartToAwayHop>();
    assembly.emplace<HomeToAwayHop>();
    assembly.emplace<AwayToAwayHop>();
    assembly.emplace<AwayToHomeHop>();
    assembly.emplace<AwayToEndHop>();
    assembly.emplace<ClassicMatches>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace sls

#endif  // SLS_CONSTRAINTS_HPP
