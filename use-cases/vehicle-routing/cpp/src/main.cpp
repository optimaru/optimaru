#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vrp/demo_data.hpp"
#include "vrp/domain.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/solver.hpp"

using namespace vrp;

namespace
{

void solveRandomInstance(int visitCount, int vehicleCount, int seconds,
                         bool ruinRecreate)
{
    std::printf(
        "== Solving a random Firenze-like instance (%d visits, %d vehicles, %ds,"
        " ALNS %s) ==\n",
        visitCount, vehicleCount, seconds, ruinRecreate ? "on" : "off");
    VehicleRoutePlan plan = buildRandomInstance(visitCount, vehicleCount, /*seed=*/2);

    plan.rebuildUnassignedPool();
    const Score initial = ScoreCalculator::calculateScore(plan);
    std::printf("  initial (all unassigned): %s\n", initial.toString().c_str());

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 2;
    config.ruinRecreate = ruinRecreate;
    config.onNewBest = [](std::int64_t step, const Score &s)
    {
        std::printf("    new best @step %-8lld %s\n", static_cast<long long>(step),
                    s.toString().c_str());
    };
    Solver(config).solve(plan);

    std::printf("  final score: %s  (feasible=%s)\n", plan.score.toString().c_str(),
                plan.score.isFeasible() ? "true" : "false");
    for (const Vehicle &vehicle : plan.vehicles)
    {
        std::printf("    vehicle %s (cap %d): %zu visits\n", vehicle.id.c_str(),
                    vehicle.capacity, vehicle.visitIndices.size());
    }
    std::printf("    unassigned: %zu visits\n", plan.unassignedVisitIndices.size());
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    // vrp_demo [visitCount] [vehicleCount] [seconds] [--no-alns]
    const int visits = (argc > 1) ? std::atoi(argv[1]) : 25;
    const int vehicles = (argc > 2) ? std::atoi(argv[2]) : 6;
    const int seconds = (argc > 3) ? std::atoi(argv[3]) : 3;
    bool ruinRecreate = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--no-alns") == 0)
            ruinRecreate = false;
    }

    solveRandomInstance(visits, vehicles, seconds, ruinRecreate);
    return 0;
}
