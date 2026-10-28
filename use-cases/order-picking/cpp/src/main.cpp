// Order-picking demo: reproduces the Java quickstart's DemoDataGenerator
// startup dataset (5 trolleys x 4 buckets x 60x40x20cm, 8 orders over the 37
// product catalog, java.util.Random(37)), solves it for a few seconds and
// prints the trolley paths — the C++ analog of pressing "Solve" in the
// quickstart's webapp.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "op/demo_data.hpp"
#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "op/solver.hpp"

using namespace op;

int main()
{
    OrderPickingSolution solution = buildDemoData();

    std::printf("Order picking demo (Java quickstart dataset)\n");
    std::printf("  trolleys: %d (buckets %d x %d cm3 each)\n",
                static_cast<int>(solution.trolleys.size()),
                solution.trolleys[0].bucketCount,
                solution.trolleys[0].bucketCapacity);
    std::printf("  orders:   %d, pick tasks: %d\n\n",
                static_cast<int>(solution.orders.size()),
                static_cast<int>(solution.pickTasks.size()));

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(10);
    // Every pick assigned and no trolley short on buckets: hard 0. Stop as
    // soon as a feasible solution is reached? No — keep polishing the travel
    // distance for the full budget, like the quickstart does.
    config.randomSeed = 0;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("  new best at step %6lld: %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver(config).solve(solution);

    const ConstraintTotals totals = ScoreCalculator::calculateTotals(solution);
    std::printf("\nFinal score: %s\n", solution.score.toString().c_str());
    std::printf("  bucket shortage (hard):    %lld\n",
                static_cast<long long>(totals.requiredNumberOfBuckets));
    std::printf(
        "  travel distance:           %lld m\n",
        static_cast<long long>(totals.distanceFromPreviousPickTask
                               + totals.distanceFromLastPickTaskToPathOrigin));
    std::printf("  order splits (x1000):      %lld\n\n",
                static_cast<long long>(totals.orderSplitByTrolley / 1000));

    for (const Trolley &trolley : solution.trolleys)
    {
        std::printf("Trolley %s  (distance %3d m, %d picks): ",
                    trolley.id.c_str(),
                    calculateDistanceToTravel(solution, trolley),
                    static_cast<int>(trolley.pickTaskIndices.size()));
        std::printf("%s", trolley.location.shelvingId().c_str());
        for (int pickIndex : trolley.pickTaskIndices)
        {
            const PickTask &task = solution.pickTasks[pickIndex];
            std::printf(" -> %s@%s",
                        task.id.c_str(),
                        task.location.shelvingId().c_str());
        }
        std::printf(" -> %s\n", trolley.location.shelvingId().c_str());
    }

    // The demo must reach a feasible plan (hard 0: every order fits in the
    // assigned trolleys' buckets) with every pick task assigned.
    std::size_t assigned = 0;
    for (const Trolley &trolley : solution.trolleys)
        assigned += trolley.pickTaskIndices.size();
    if (!solution.score.isFeasible() || assigned != solution.pickTasks.size())
    {
        std::printf("\nFAILED: expected a feasible, fully assigned plan "
                    "(got %s, %d/%d assigned)\n",
                    solution.score.toString().c_str(),
                    static_cast<int>(assigned),
                    static_cast<int>(solution.pickTasks.size()));
        return EXIT_FAILURE;
    }
    std::printf("\nFeasible plan: every pick assigned, no bucket shortage.\n");
    return EXIT_SUCCESS;
}
