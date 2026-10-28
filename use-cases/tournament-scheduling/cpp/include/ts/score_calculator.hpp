#ifndef TS_SCORE_CALCULATOR_HPP
#define TS_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ts/domain.hpp"

namespace ts
{

// Port of the Timefold tournament-scheduling
// TournamentScheduleConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (Java HardMediumSoftBigDecimalScore, mapped per ts::Score):
//   hard 1. oneAssignmentPerDatePerTeam  -1 per assignment pair with the same
//                                        team on the same day (C(c, 2) per
//                                        (team, day) cell)
//   hard 2. unavailabilityPenalty        -1 per UnavailabilityPenalty fact
//                                        whose (team, day) has >= 1 assignment
//   med  3. fairAssignmentCountPerTeam   -unfairness of the per-team
//                                        assignment counts (teams with >= 1
//                                        assignment; no complement in Java)
//   soft 4. evenlyConfrontationCount     -unfairness of the confrontation
//                                        counts per team pair: one
//                                        confrontation per same-day
//                                        assignment pair with teamId1 <
//                                        teamId2 (strict: same-team pairs
//                                        excluded)
//
// "unfairness" is Timefold's LoadBalance::unfairness: sqrt(sum(load^2) -
// sum(load)^2 / n) over the n currently loaded items, rounded to 6
// significant figures HALF_EVEN (BigDecimal.sqrt with MathContext(6)).
// Unassigned assignments (team == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score calculateScore(TournamentSchedule &schedule);

    // --- shared building blocks (also used by the incremental calculator) ---

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

    // Timefold LoadBalance::unfairness from the running aggregates:
    // sqrt(sumOfSquares - sumOfLoads^2 / itemCount), 0 when itemCount == 0,
    // rounded to 6 significant figures HALF_EVEN like the BigDecimal sqrt.
    static double unfairnessFromAggregates(std::int64_t sumOfLoads,
                                           std::int64_t sumOfSquares,
                                           int itemCount);

    static double unfairness(const std::vector<std::int64_t> &loads);

    // The one shared soft assembly (medium folded per ts::kMediumScale); both
    // calculators call this so FULL_ASSERT compares bit-identical doubles.
    static double combinedSoft(double mediumUnfairness, double softUnfairness)
    {
        const double penalty = mediumUnfairness * kMediumScale + softUnfairness;
        return penalty == 0.0 ? 0.0 : -penalty;  // normalize -0.0
    }
};

}  // namespace ts

#endif  // TS_SCORE_CALCULATOR_HPP
