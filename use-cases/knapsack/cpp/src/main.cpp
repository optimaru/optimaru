// Demo: the web app's knapsack sample (camping trip, 12 items, capacity 60,
// two fragile slots, a mandatory first-aid kit, at most one of tent / drone,
// and the drone needs its battery), solved for two seconds.
//
//   ks_demo [seconds]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ks/constraints.hpp"
#include "ks/domain.hpp"
#include "ks/score_calculator.hpp"
#include "ks/solver.hpp"

using namespace ks;

namespace
{

Item makeItem(const std::string &id, std::int64_t weight, std::int64_t value)
{
    Item item;
    item.id = id;
    item.weight = weight;
    item.value = value;
    return item;
}

KnapsackProblem campingTrip()
{
    KnapsackProblem problem;
    problem.capacity = 60;
    problem.resources.push_back({"fragile_slots", 2});
    problem.items = {
        makeItem("camera", 12, 24),  makeItem("laptop", 18, 38), makeItem("battery", 8, 18),
        makeItem("tent", 22, 35),    makeItem("water", 15, 20),  makeItem("food", 10, 22),
        makeItem("jacket", 9, 16),   makeItem("first-aid", 6, 15), makeItem("drone", 20, 40),
        makeItem("book", 5, 8),      makeItem("stove", 11, 21),  makeItem("radio", 7, 14),
    };
    problem.items[0].resourceUsage["fragile_slots"] = 1;
    problem.items[8].resourceUsage["fragile_slots"] = 2;
    problem.items[8].requiredItems = {2};  // the drone requires the battery
    problem.mandatoryItems = {7};          // first-aid
    SelectionGroup largeGear;
    largeGear.id = "large-gear";
    largeGear.items = {3, 8};  // tent, drone
    largeGear.maxSelected = 1;
    problem.selectionGroups.push_back(largeGear);
    return problem;
}

}  // namespace

int main(int argc, char **argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    KnapsackProblem problem = campingTrip();

    Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    config.randomSeed = 1;
    Solver solver(config);
    solver.solve(problem);

    std::printf("Score: %s\n", problem.score.toString().c_str());
    std::printf("Selected (weight %lld / %lld, value %lld):",
                static_cast<long long>(problem.totalWeight()),
                static_cast<long long>(problem.capacity),
                static_cast<long long>(problem.totalValue()));
    for (const Item &item : problem.items)
    {
        if (item.isSelected())
            std::printf(" %s", item.id.c_str());
    }
    std::printf("\n");

    // The same solution explained per constraint by the default assembly.
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(problem);
    calculator.calculateScore();
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        std::printf("  %-26s %-4s %g\n", total.name.c_str(), optimaru::levelName(total.level),
                    total.cost);
    }
    return 0;
}
