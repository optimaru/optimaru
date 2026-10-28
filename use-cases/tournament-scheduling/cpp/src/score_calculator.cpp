#include "ts/score_calculator.hpp"

#include <cmath>
#include <cstddef>

namespace ts
{

double ScoreCalculator::unfairnessFromAggregates(std::int64_t sumOfLoads,
                                                 std::int64_t sumOfSquares,
                                                 int itemCount)
{
    if (itemCount == 0)
        return 0.0;
    // ssd = sum(load^2) - sum(load)^2 / n (identical to the n==1 special
    // case, where it is always 0).
    double ssd = static_cast<double>(sumOfSquares)
                 - (static_cast<double>(sumOfLoads) * sumOfLoads)
                       / static_cast<double>(itemCount);
    if (ssd < 0)
        ssd = 0;
    const double root = std::sqrt(ssd);
    if (root == 0.0)
        return 0.0;
    // Round to 6 significant figures, HALF_EVEN — the BigDecimal.sqrt
    // MathContext of Timefold's LoadBalanceImpl. Avoid log10()/pow() in the
    // per-move hot path: the loop finds ceil(log10(root)) with comparisons.
    int digitsBeforePoint = 0;
    double magnitude = 1.0;
    if (root > 1.0)
    {
        while (magnitude < root)
        {
            magnitude *= 10.0;
            ++digitsBeforePoint;
        }
    }
    else
    {
        while (magnitude / 10.0 >= root)
        {
            magnitude /= 10.0;
            --digitsBeforePoint;
        }
    }
    double factor = 1.0;
    const int exponent = 6 - digitsBeforePoint;
    if (exponent >= 0)
    {
        for (int i = 0; i < exponent; ++i)
            factor *= 10.0;
    }
    else
    {
        for (int i = 0; i > exponent; --i)
            factor /= 10.0;
    }
    const double scaled = root * factor;
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    const double rounded
        = fraction > 0.5 || (fraction == 0.5 && std::fmod(lower, 2.0) != 0.0)
              ? lower + 1.0
              : lower;
    return rounded / factor;
}

double ScoreCalculator::unfairness(const std::vector<std::int64_t> &loads)
{
    std::int64_t sum = 0, sumSq = 0;
    for (std::int64_t v : loads)
    {
        sum += v;
        sumSq += v * v;
    }
    return unfairnessFromAggregates(sum, sumSq, static_cast<int>(loads.size()));
}

Score ScoreCalculator::calculateScore(TournamentSchedule &schedule)
{
    const int teamCount = schedule.teamCount();
    const int dayCount = schedule.dayCount();
    Score score;

    // Occupancy per (team, day) cell.
    std::vector<int> cellCnt(static_cast<std::size_t>(teamCount) * dayCount, 0);
    std::vector<std::int64_t> teamLoad(teamCount, 0);
    for (const TeamAssignment &assignment : schedule.teamAssignments)
    {
        if (assignment.team < 0)
            continue;  // unassigned contributes nothing
        ++cellCnt[static_cast<std::size_t>(assignment.team) * dayCount
                  + assignment.day];
        ++teamLoad[assignment.team];
    }

    // hard 1: oneAssignmentPerDatePerTeam — C(c, 2) per cell.
    std::int64_t hard = 0;
    for (int c : cellCnt)
        hard += pairs(c);

    // hard 2: unavailabilityPenalty — one per fact with a hit cell.
    for (const UnavailabilityPenalty &penalty :
         schedule.unavailabilityPenalties)
    {
        if (cellCnt[static_cast<std::size_t>(penalty.team) * dayCount
                    + penalty.day]
            > 0)
            ++hard;
    }

    // medium 3: fairAssignmentCountPerTeam — unfairness of the loads of the
    // teams with >= 1 assignment (Java has no complement here).
    std::int64_t balSum = 0, balSumSq = 0;
    int balN = 0;
    for (int t = 0; t < teamCount; ++t)
    {
        if (teamLoad[t] == 0)
            continue;
        balSum += teamLoad[t];
        balSumSq += teamLoad[t] * teamLoad[t];
        ++balN;
    }
    const double medium = unfairnessFromAggregates(balSum, balSumSq, balN);

    // soft 4: evenlyConfrontationCount — per day, each assignment pair with
    // teamId1 < teamId2 is one confrontation of that team pair; unfairness
    // of the per-pair confrontation counts (pairs with >= 1 confrontation).
    std::int64_t confSum = 0, confSumSq = 0;
    int confN = 0;
    for (int t1 = 0; t1 < teamCount; ++t1)
    {
        for (int t2 = t1 + 1; t2 < teamCount; ++t2)
        {
            std::int64_t confrontations = 0;
            for (int d = 0; d < dayCount; ++d)
            {
                confrontations
                    += static_cast<std::int64_t>(
                           cellCnt[static_cast<std::size_t>(t1) * dayCount + d])
                       * cellCnt[static_cast<std::size_t>(t2) * dayCount + d];
            }
            if (confrontations == 0)
                continue;
            confSum += confrontations;
            confSumSq += confrontations * confrontations;
            ++confN;
        }
    }
    const double soft = unfairnessFromAggregates(confSum, confSumSq, confN);

    score.hard = -hard;
    score.soft = combinedSoft(medium, soft);
    schedule.score = score;
    return score;
}

}  // namespace ts
