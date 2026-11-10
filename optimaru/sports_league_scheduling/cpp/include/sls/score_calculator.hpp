#ifndef SLS_SCORE_CALCULATOR_HPP
#define SLS_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "sls/domain.hpp"

namespace sls
{

// Per-constraint raw totals of one full recompute (all counted positive; the
// score applies the signs). Mirrors the ten constraints of the quickstart's
// SportsLeagueSchedulingConstraintProvider one to one; each member is named
// after its constraint's registry name (sls/constraints/<subject>_cost.hpp).
// The weights are applied only in hardTotal() / softTotal(), which read each
// component's kDefaultWeight (score_calculator.cpp).
struct Breakdown
{
    // Hard (counts).
    std::int64_t teamConflictCost = 0;
    std::int64_t consecutiveHomeMatchesCost = 0;  // sum of run lengths >= 4
    std::int64_t consecutiveAwayMatchesCost = 0;  // sum of run lengths >= 4
    std::int64_t repeatMatchOnNextDayCost = 0;
    // Soft (distance sums in km, plus the workday classic-match count).
    std::int64_t startToAwayHopCost = 0;
    std::int64_t homeToAwayHopCost = 0;
    std::int64_t awayToAwayHopCost = 0;
    std::int64_t awayToHomeHopCost = 0;
    std::int64_t awayToEndHopCost = 0;
    std::int64_t workdayClassicMatchesCost = 0;  // weighted 1000 each

    // Weighted totals at the components' kDefaultWeight (all integral, so the
    // sums are exact int64).
    std::int64_t hardTotal() const;
    std::int64_t softTotal() const;
};

// Port of the reference sports-league-scheduling
// SportsLeagueSchedulingConstraintProvider.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (Score; round adjacency is by Round::index), each at
// its component's default weight:
//   hard 1. teamConflictCost          -1 per match pair in the same round
//                                        sharing at least one team (a pair
//                                        sharing both teams still counts once)
//   hard 2. consecutiveHomeMatchesCost  -n per team whose home matches cover a
//                                        consecutive round-index run of n >= 4
//                                        distinct indices (several matches on
//                                        one index count once: the quickstart's
//                                        toConsecutiveSequences collects the
//                                        Round values into a set)
//   hard 3. consecutiveAwayMatchesCost  same for away matches
//   hard 4. repeatMatchOnNextDayCost  -1 per match followed on round index + 1
//                                        by the reversed fixture (ifExists:
//                                        at most once per match)
//   soft 5. startToAwayHopCost        -dist(away, home) per match in a round
//                                        with no previous round index
//   soft 6. homeToAwayHopCost         -dist(m1.home, m2.home) per match pair
//                                        (m1, m2) on consecutive round indices
//                                        with m2.away == m1.home
//   soft 7. awayToAwayHopCost         -dist(m1.home, m2.home) per pair with
//                                        m2.away == m1.away
//   soft 8. awayToHomeHopCost         -dist(m1.home, m1.away) per pair with
//                                        m2.home == m1.away
//   soft 9. awayToEndHopCost          -dist(home, away) per match in a round
//                                        with no next round index
//   soft 10. workdayClassicMatchesCost  -1000 per classic match in a round
//                                        that is not a weekend/holiday
// Unassigned matches (round == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    // A run of >= MAX_CONSECUTIVE consecutive home (or away) round indices
    // is penalized by its length (the quickstart's MAX_CONSECUTIVE_MATCHES).
    static constexpr int MAX_CONSECUTIVE = 4;

    static Score score(LeagueSchedule &schedule);
    static Breakdown computeBreakdown(LeagueSchedule &schedule);

    // --- shared building blocks (also used by the plugin components) ---

    // Penalty of one team side (home or away): the summed length of every
    // maximal consecutive round-index run covering >= MAX_CONSECUTIVE
    // distinct indices. `cnt` is the per-round-index match count of that
    // (team, side); only presence matters — the reference
    // model collects the Round values into a set before finding
    // consecutive sequences, so several matches on one round index count
    // once, exactly like a duplicated Round fact would.
    static std::int64_t runPenalty(const std::vector<int> &cnt);

    // Interns the ordered (home, away) team pairs into dense ids, resolves
    // each match's reversed-pair id, fills the round-index existence table
    // and zero-fills a missing distance matrix. Idempotent (guarded by
    // LeagueSchedule::prepared). Must run before scoring; score /
    // solve call it.
    static void prepare(LeagueSchedule &schedule);
};

}  // namespace sls

#endif  // SLS_SCORE_CALCULATOR_HPP
