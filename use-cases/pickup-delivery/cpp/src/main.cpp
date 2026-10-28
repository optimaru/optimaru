#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"

using namespace pdp;

int main(int argc, char **argv)
{
    // pdp_demo [requestCount] [vehicleCount] [seconds] [--no-pair-moves]
    const int requests = (argc > 1) ? std::atoi(argv[1]) : 30;
    const int vehicles = (argc > 2) ? std::atoi(argv[2]) : 5;
    const int seconds = (argc > 3) ? std::atoi(argv[3]) : 5;
    bool pairMoves = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--no-pair-moves") == 0)
            pairMoves = false;
    }

    std::printf(
        "== PDPTW: %d requests, %d vehicles, %ds (pair operators %s) ==\n",
        requests,
        vehicles,
        seconds,
        pairMoves ? "on" : "off");
    PdpRoutePlan plan = buildRandomInstance(requests, vehicles, /*seed=*/2);

    plan.rebuildUnassignedPool();
    std::printf("  initial (all unassigned): %s\n",
                ScoreCalculator::calculateScore(plan).toString().c_str());

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 2;
    config.pairMoves = pairMoves;
    config.pairRuin = pairMoves;
    config.onNewBest = [](std::int64_t step, const Score &s)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    s.toString().c_str());
    };
    Solver(config).solve(plan);

    std::printf("  final score: %s  (feasible=%s)\n",
                plan.score.toString().c_str(),
                plan.score.isFeasible() ? "true" : "false");
    for (const Vehicle &vehicle : plan.vehicles)
    {
        std::printf("    vehicle %s (cap %d): %zu stops\n",
                    vehicle.id.c_str(),
                    vehicle.capacity,
                    vehicle.nodeIndices.size());
    }
    int dropped = 0;
    for (const Delivery &delivery : plan.deliveries)
    {
        if (plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)]
                .vehicleIndex
            < 0)
            ++dropped;
    }
    std::printf(
        "    unrouted requests: %d / %zu\n", dropped, plan.deliveries.size());
    return 0;
}
