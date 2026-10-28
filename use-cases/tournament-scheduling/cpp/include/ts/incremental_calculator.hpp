#ifndef TS_INCREMENTAL_CALCULATOR_HPP
#define TS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "ts/domain.hpp"

namespace ts
{

// The tournament-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is a function of per-(team, day) occupancy counts and of
// two load-balance aggregate triples (sum, sum of squares, item count), so a
// single variable change is O(teamCount):
//   - hard 1 (oneAssignmentPerDatePerTeam): C(c, 2) pair sums per cell;
//     moving one assignment in/out of a cell with c other members changes
//     the pair count by exactly c.
//   - hard 2 (unavailabilityPenalty): per-cell penalty-fact counts; the hit
//     total changes only when a cell's occupancy crosses 0 <-> 1.
//   - medium 3 (fairAssignmentCountPerTeam): the (sum, sumSq, n) aggregates
//     of the per-team loads; one load changing by 1 is an O(1) update
//     (n counts only teams with load >= 1 — Java has no complement here).
//   - soft 4 (evenlyConfrontationCount): per-team-pair confrontation counts;
//     inserting an assignment of team t on day d adds cellCnt[u][d]
//     confrontations to pair (t, u) for every other team u — the O(teamCount)
//     part — with the aggregates updated per touched pair.
//
// beforeVariableChanged retracts the assignment's contribution while `team`
// still holds the OLD value; afterVariableChanged inserts the NEW value.
// Unassigned assignments (team == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in
// the tests and under EnvironmentMode::FULL_ASSERT); both assemble the final
// double through ScoreCalculator::combinedSoft, so the comparison is exact.
class TournamentSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<TournamentSchedule>
{
public:
    void resetWorkingSolution(TournamentSchedule &workingSolution) override;
    void beforeVariableChanged(int assignmentIndex) override;
    void afterVariableChanged(int assignmentIndex) override;
    Score calculateScore() override;

    // --- live state for the solver's guided moves (all O(1) reads) ---
    // The load-balance levels dominate this domain's score, so the custom
    // moves steer by the very aggregates maintained here instead of
    // rescanning the solution: the per-team assignment counts, how many teams
    // are currently loaded (the `n` of medium 3's unfairness) and the
    // per-(team, day) occupancy / penalty-fact counters that say whether a
    // team can take an assignment on a day without breaking a hard
    // constraint.
    const std::vector<std::int64_t> &teamLoads() const { return teamLoad_; }
    int loadedTeamCount() const { return balN_; }
    int cellCount(int team, int day) const
    {
        return cellCnt_[cellOffset(team, day)];
    }
    int penaltyCount(int team, int day) const
    {
        return penaltyCnt_[cellOffset(team, day)];
    }

private:
    TournamentSchedule *schedule_ = nullptr;
    int teamCount_ = 0;
    int dayCount_ = 0;
    std::vector<int> assign_;  // team per assignment (-1 == unassigned)

    // Occupancy and penalty-fact counters.
    std::vector<int> cellCnt_;     // (team, day) -> assignment count
    std::vector<int> penaltyCnt_;  // (team, day) -> UnavailabilityPenalty facts
    std::vector<std::int64_t> teamLoad_;  // team -> assignment count
    std::vector<std::int64_t> confCnt_;   // (t1, t2), t1 < t2 -> confrontations

    // Running totals (counted positive; signs applied in calculateScore).
    std::int64_t pairSum_ = 0;     // hard 1
    std::int64_t unavailHit_ = 0;  // hard 2
    std::int64_t balSum_ = 0;      // medium 3 aggregates
    std::int64_t balSumSq_ = 0;
    int balN_ = 0;
    std::int64_t confSum_ = 0;  // soft 4 aggregates
    std::int64_t confSumSq_ = 0;
    int confN_ = 0;

    void insert(int assignmentIndex, int team);
    void retract(int assignmentIndex, int team);
    void addConfrontations(int t1, int t2, std::int64_t delta);

    std::size_t cellOffset(int team, int day) const
    {
        return static_cast<std::size_t>(team) * dayCount_ + day;
    }
    std::size_t pairOffset(int t1, int t2) const  // requires t1 < t2
    {
        return static_cast<std::size_t>(t1) * teamCount_ + t2;
    }
};

}  // namespace ts

#endif  // TS_INCREMENTAL_CALCULATOR_HPP
