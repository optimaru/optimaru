#include "fl/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fl
{

void ScoreCalculator::prepare(FacilityLocationProblem &problem)
{
    if (problem.prepared)
        return;

    const int facilityCount = problem.facilityCount();
    problem.distanceMeters.assign(
        static_cast<std::size_t>(problem.consumerCount()) * facilityCount, 0);
    for (int c = 0; c < problem.consumerCount(); ++c)
    {
        const Location &consumerLocation = problem.consumers[c].location;
        for (int f = 0; f < facilityCount; ++f)
        {
            problem
                .distanceMeters[static_cast<std::size_t>(c) * facilityCount + f]
                = problem.facilities[f].location.distanceTo(consumerLocation);
        }
    }

    problem.prepared = true;
}

Score ScoreCalculator::calculateScore(FacilityLocationProblem &problem)
{
    prepare(problem);

    // Per-facility aggregates of the assigned consumers — the C++ stand-in
    // for the Java Facility's inverse-relation `consumers` shadow list.
    std::vector<std::int64_t> usedDemand(problem.facilities.size(), 0);
    std::vector<int> consumerCnt(problem.facilities.size(), 0);

    std::int64_t distance = 0;  // soft 3 (unweighted meters)
    for (int c = 0; c < problem.consumerCount(); ++c)
    {
        const Consumer &consumer = problem.consumers[c];
        if (!consumer.isAssigned())
            continue;  // unassigned: contributes nothing
        usedDemand[consumer.facility] += consumer.demand;
        ++consumerCnt[consumer.facility];
        distance += problem.distanceOf(c, consumer.facility);
    }

    // hard 1: facilityCapacity — Java groups consumers by facility, so only
    // facilities with at least one assigned consumer are penalized.
    std::int64_t overCapacity = 0;
    // soft 2: facilitySetupCost — each used facility once (unweighted).
    std::int64_t setupCost = 0;
    for (std::size_t f = 0; f < problem.facilities.size(); ++f)
    {
        if (consumerCnt[f] == 0)
            continue;
        overCapacity += std::max<std::int64_t>(
            0, usedDemand[f] - problem.facilities[f].capacity);
        setupCost += problem.facilities[f].setupCost;
    }

    Score score;
    score.hard = -overCapacity;
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(
        -(problem.setupCostWeight * setupCost
          + problem.distanceFromFacilityWeight * distance));
    problem.score = score;
    return score;
}

}  // namespace fl
