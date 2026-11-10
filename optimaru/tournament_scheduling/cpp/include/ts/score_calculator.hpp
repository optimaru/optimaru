#ifndef TS_SCORE_CALCULATOR_HPP
#define TS_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ts/domain.hpp"

namespace ts
{

// The four tournament-scheduling constraints.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (hard / medium / soft levels, mapped per ts::Score;
// default weights per unit are the components' kDefaultWeight, see
// ts/constraints/<subject>_cost.hpp):
//   hard 1. teamConflictCost             -1 per assignment pair with the same
//                                        team on the same day (C(c, 2) per
//                                        (team, day) cell)
//   hard 2. teamUnavailabilityCost       -1 per UnavailabilityPenalty fact
//                                        whose (team, day) has >= 1 assignment
//   med  3. assignmentCountImbalanceCost -unfairness of the per-team
//                                        assignment counts (teams with >= 1
//                                        assignment; unloaded teams excluded)
//   soft 4. confrontationCountImbalanceCost
//                                        -unfairness of the confrontation
//                                        counts per team pair: one
//                                        confrontation per same-day
//                                        assignment pair with teamId1 <
//                                        teamId2 (strict: same-team pairs
//                                        excluded)
//
// "unfairness" is the load-balance unfairness: sqrt(sum(load^2) -
// sum(load)^2 / n) over the n currently loaded items, rounded to 6
// significant figures HALF_EVEN.
// Unassigned assignments (team == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score score(TournamentSchedule &schedule);

    // --- shared building blocks (also used by the incremental calculator) ---

    // Unordered pairs among n items: C(n, 2).
    static std::int64_t pairs(std::int64_t n) { return n * (n - 1) / 2; }

    // Load-balance unfairness from the running aggregates:
    // sqrt(sumOfSquares - sumOfLoads^2 / itemCount), 0 when itemCount == 0,
    // rounded to 6 significant figures HALF_EVEN.
    static double unfairnessFromAggregates(std::int64_t sumOfLoads,
                                           std::int64_t sumOfSquares,
                                           int itemCount);

    static double unfairness(const std::vector<std::int64_t> &loads);

    // The one shared soft assembly: the weighted medium and soft penalties
    // (counted positive) folded with the domain's kMediumFold, i.e.
    // -(soft + medium * 1e6) — the same arithmetic the decomposed calculator
    // applies to its medium accumulator. Both calculators call this so
    // ScoreChecks::Full compares bit-identical doubles.
    static double combinedSoft(double mediumPenalty, double softPenalty)
    {
        const double soft
            = kMediumFold.to_engine(0, -mediumPenalty, -softPenalty).soft;
        return soft == 0.0 ? 0.0 : soft;  // normalize -0.0
    }
};

}  // namespace ts

#endif  // TS_SCORE_CALCULATOR_HPP
