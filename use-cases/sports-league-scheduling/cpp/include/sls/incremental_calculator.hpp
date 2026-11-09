#ifndef SLS_INCREMENTAL_CALCULATOR_HPP
#define SLS_INCREMENTAL_CALCULATOR_HPP

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "sls/domain.hpp"
#include "optimaru/objective.hpp"

namespace sls
{

// The sports-league-scheduling implementation of the engine's
// optimaru::Objective — the C++ analog of implementing
// Java's Objective instead of a ConstraintProvider.
//
// Every constraint couples a match only to matches on the same or an
// adjacent round index, so a single variable change is O(delta):
//   - hard 1 (teamConflictCost) and soft 6-8 (the hop pairs
//     homeToAwayHopCost / awayToAwayHopCost / awayToHomeHopCost): scan the
//     match buckets of the old/new round index and its two neighbors
//     (roundMatches_).
//   - hard 2/3 (consecutiveHomeMatchesCost / consecutiveAwayMatchesCost):
//     the change touches one round index of one (team, side) count row; the
//     affected maximal run is re-priced in place (bounded by the run's
//     length).
//   - hard 4 (repeatMatchOnNextDayCost): ordered-pair-per-round-index
//     counters give both the match's own predicate and the flips it causes
//     on reversed fixtures of the previous round index.
//   - soft 5/9 (startToAwayHopCost / awayToEndHopCost) and soft 10
//     (workdayClassicMatchesCost): O(1) lookups.
//
// score weights the running totals with the components'
// kDefaultWeight (the default assembly, computed in int64).
//
// before_assign retracts the match's contribution while `round`
// still holds the OLD value; after_assign inserts the NEW value.
// Unassigned matches (round == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions. The full-recompute
// reference is ScoreCalculator::score (checked in the tests and
// under ScoreChecks::ScoreChecks::Full).
class SportsLeagueIncrementalScoreCalculator final
    : public optimaru::Objective<LeagueSchedule>
{
public:
    void rebuild(LeagueSchedule &solution) override;
    void before_assign(int matchIndex) override;
    void after_assign(int matchIndex) override;
    Score score() override;

    // Tournament selection on the per-match soft cost (see matchCost_): the
    // most expensive of `sample` randomly drawn matches. Drives the solver's
    // travel-guided entity selection; -1 when there is nothing to pick.
    int costliestMatch(std::mt19937_64 &rng, int sample) const;

    // The soft penalty a single match currently carries (see matchCost_).
    // Summed over all matches this is the soft total with the three hop-pair
    // terms counted twice, once for each match of the pair — the invariant
    // the tests check.
    std::int64_t matchCost(int matchIndex) const
    {
        return matchCost_[static_cast<std::size_t>(matchIndex)];
    }

private:
    LeagueSchedule *schedule_ = nullptr;
    int indexCount_ = 0;       // maxRoundIndex + 1
    std::vector<int> assign_;  // round (value) per match; -1 == unassigned

    // Assigned matches bucketed by round index, with O(1) removal.
    std::vector<std::vector<int>> roundMatches_;
    std::vector<int> posInRound_;

    // Occupancy counters (same shapes as the full recompute's locals).
    std::vector<int> homeCnt_;  // team x roundIndex
    std::vector<int> awayCnt_;  // team x roundIndex
    std::vector<int> pairCnt_;  // orderedPairId x roundIndex

    // Raw running totals (all counted positive; weights and signs applied in
    // score).
    std::int64_t teamConflict_ = 0;  // hard 1, teamConflictCost
    std::int64_t consecutive_ = 0;   // hard 2 + 3, consecutive*MatchesCost
    std::int64_t repeat_ = 0;        // hard 4, repeatMatchOnNextDayCost
    std::int64_t startHop_ = 0;      // soft 5, startToAwayHopCost
    std::int64_t homeAwayHop_ = 0;   // soft 6, homeToAwayHopCost
    std::int64_t awayAwayHop_ = 0;   // soft 7, awayToAwayHopCost
    std::int64_t awayHomeHop_ = 0;   // soft 8, awayToHomeHopCost
    std::int64_t endHop_ = 0;        // soft 9, awayToEndHopCost
    std::int64_t classic_ = 0;       // soft 10, workdayClassicMatchesCost
                                     // (count; weighted 1000)

    // Soft penalty each match currently carries: its own start/end hop and
    // classic penalty plus every hop pair it takes part in, credited to both
    // matches of the pair, each term at its component's kDefaultWeight (as in
    // score). Updated by exactly the same deltas as the running
    // totals above, so it needs no extra scans; an unassigned match is 0.
    // Read only by the guided selection (costliestMatch) — never by the score.
    std::vector<std::int64_t> matchCost_;

    void insert(int matchIndex, int round);
    void retract(int matchIndex, int round);

    // Applies `delta` (+1/-1) to cnt[team][roundIndex] and updates
    // consecutive_ by re-pricing the maximal run around roundIndex.
    void bumpRun(std::vector<int> &cnt, int team, int roundIndex, int delta);

    std::size_t pairRoundOffset(int pairId, int roundIndex) const
    {
        return static_cast<std::size_t>(pairId) * indexCount_ + roundIndex;
    }
};

}  // namespace sls

#endif  // SLS_INCREMENTAL_CALCULATOR_HPP
