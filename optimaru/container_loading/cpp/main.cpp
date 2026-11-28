// Container-loading demo: builds the demo pallets (40 boxes of six kinds, four pallets of
// 1200 x 800 x 1500 mm, seed 37), solves it for a few seconds and prints where every box stands.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "cl/model/demo_data.hpp"
#include "cl/model/domain.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/solver.hpp"

using namespace cl;

int main() {
    ContainerLoadingProblem problem = generateDemoData();

    std::printf("Container loading demo\n");
    std::printf("  containers: %d (%d x %d x %d mm)\n", static_cast<int>(problem.containers.size()),
                problem.containers[0].length, problem.containers[0].width,
                problem.containers[0].height);
    std::printf("  items:      %d\n\n", static_cast<int>(problem.items.size()));

    Solver::Config config;
    config.time_limit = std::chrono::seconds(5);
    config.seed = 0;
    config.on_new_best = [](std::int64_t step, const Score& score) {
        std::printf("  new best at step %6lld: %s\n", static_cast<long long>(step),
                    score.str().c_str());
    };
    Solver(config).solve(problem);

    const ConstraintTotals totals = ScoreCalculator::calculateTotals(problem);
    std::printf("\nFinal score: %s\n", problem.score.str().c_str());
    std::printf("  unplaced items (hard):       %lld\n",
                static_cast<long long>(totals.unplacedItemCost));
    std::printf("  weight above limits (hard):  %lld g\n",
                static_cast<long long>(totals.containerWeightCost));
    std::printf("  stacking overload (hard):    %lld g\n",
                static_cast<long long>(totals.stackingLoadCost));
    std::printf("  containers in use:           %d\n", problem.containersUsed());
    std::printf("  groups split:                %lld\n\n",
                static_cast<long long>(totals.groupSplitCost));

    for (const Container& container : problem.containers) {
        if (container.placedItems.empty()) continue;
        std::printf("%s  (%d items, %.0f%% of the volume, %lld kg)\n", container.id.c_str(),
                    static_cast<int>(container.placedItems.size()), 100.0 * container.fillRatio(),
                    static_cast<long long>(container.loadedWeight / 1000));
        for (int index : container.placedItems) {
            const Item& item = problem.items[static_cast<std::size_t>(index)];
            std::printf("  %2d. %-12s at (%4d, %4d, %4d)  %4d x %4d x %4d\n", item.sequence + 1,
                        item.id.c_str(), item.x, item.y, item.z, item.placedLength,
                        item.placedWidth, item.placedHeight);
        }
    }

    // The demo must reach a feasible plan: every box placed, no limit broken.
    if (!problem.score.feasible()) {
        std::printf("\nFAILED: expected a feasible loading plan (got %s)\n",
                    problem.score.str().c_str());
        return EXIT_FAILURE;
    }
    std::printf("\nFeasible plan: every box stands somewhere, no limit broken.\n");
    return EXIT_SUCCESS;
}
