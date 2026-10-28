#include "op/score_calculator.hpp"

#include <vector>

namespace op
{

void ScoreCalculator::updateShadowVariables(OrderPickingSolution &solution)
{
    for (PickTask &task : solution.pickTasks)
    {
        task.trolleyIndex = -1;
        task.previousPickTask = -1;
        task.nextPickTask = -1;
    }
    for (int t = 0; t < static_cast<int>(solution.trolleys.size()); ++t)
    {
        const std::vector<int> &list = solution.trolleys[t].pickTaskIndices;
        for (int p = 0; p < static_cast<int>(list.size()); ++p)
        {
            PickTask &task = solution.pickTasks[list[p]];
            task.trolleyIndex = t;
            task.previousPickTask = p > 0 ? list[p - 1] : -1;
            task.nextPickTask
                = p + 1 < static_cast<int>(list.size()) ? list[p + 1] : -1;
        }
    }
}

ConstraintTotals
ScoreCalculator::calculateTotals(OrderPickingSolution &solution)
{
    updateShadowVariables(solution);

    const int trolleyCount = static_cast<int>(solution.trolleys.size());
    const int orderCount = static_cast<int>(solution.orders.size());
    ConstraintTotals totals;

    // --- soft: distance constraints, reading the shadow variables like the
    // Java constraint lambdas do -------------------------------------------
    for (const PickTask &task : solution.pickTasks)
    {
        if (task.trolleyIndex == -1)
            continue;  // unassigned: forEach(PickTask) skips it
        const Trolley &trolley = solution.trolleys[task.trolleyIndex];

        // minimizeDistanceFromPreviousPickTask
        const WarehouseLocation &previousLocation
            = task.previousPickTask != -1
                  ? solution.pickTasks[task.previousPickTask].location
                  : trolley.location;
        totals.distanceFromPreviousPickTask
            += calculateDistance(previousLocation, task.location);

        // minimizeDistanceFromLastPickTaskToPathOrigin
        if (task.isLast())
        {
            totals.distanceFromLastPickTaskToPathOrigin
                += calculateDistance(task.location, trolley.location);
        }
    }

    // --- hard: requiredNumberOfBuckets, soft: minimizeOrderSplitByTrolley --
    // volume[trolley][order] = raw total volume of the order's picks on the
    // trolley (the Java groupBy(trolley, order, sum(volume)) table).
    std::vector<std::int64_t> volume(
        static_cast<std::size_t>(trolleyCount) * orderCount, 0);
    std::vector<char> orderOnTrolley(
        static_cast<std::size_t>(trolleyCount) * orderCount, 0);
    for (int t = 0; t < trolleyCount; ++t)
    {
        for (int pickIndex : solution.trolleys[t].pickTaskIndices)
        {
            const PickTask &task = solution.pickTasks[pickIndex];
            const std::size_t cell
                = static_cast<std::size_t>(t) * orderCount + task.orderIndex;
            volume[cell] += task.volume;
            orderOnTrolley[cell] = 1;
        }
    }
    for (int t = 0; t < trolleyCount; ++t)
    {
        const Trolley &trolley = solution.trolleys[t];
        std::int64_t requiredBuckets = 0;
        for (int o = 0; o < orderCount; ++o)
        {
            const std::size_t cell
                = static_cast<std::size_t>(t) * orderCount + o;
            if (orderOnTrolley[cell])
            {
                requiredBuckets += calculateOrderRequiredBuckets(
                    volume[cell], trolley.bucketCapacity);
            }
        }
        if (requiredBuckets > trolley.bucketCount)
            totals.requiredNumberOfBuckets
                += requiredBuckets - trolley.bucketCount;
    }
    for (int o = 0; o < orderCount; ++o)
    {
        std::int64_t distinctTrolleys = 0;
        for (int t = 0; t < trolleyCount; ++t)
        {
            distinctTrolleys
                += orderOnTrolley[static_cast<std::size_t>(t) * orderCount + o];
        }
        totals.orderSplitByTrolley += distinctTrolleys * 1000;
    }

    return totals;
}

Score ScoreCalculator::calculateScore(OrderPickingSolution &solution)
{
    solution.score = calculateTotals(solution).toScore();
    return solution.score;
}

}  // namespace op
