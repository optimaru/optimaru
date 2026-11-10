#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"

using namespace pdp;

int main(int argc, char** argv) {
    // pdp_demo [requestCount] [vehicleCount] [seconds] [--stop-moves-only]
    // --stop-moves-only: the control run, with every request-level operator off.
    const int requests = (argc > 1) ? std::atoi(argv[1]) : 30;
    const int vehicles = (argc > 2) ? std::atoi(argv[2]) : 5;
    const int seconds = (argc > 3) ? std::atoi(argv[3]) : 5;
    bool request_operators = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stop-moves-only") == 0) request_operators = false;
    }

    std::printf("== PDPTW: %d requests, %d vehicles, %ds (request operators %s) ==\n", requests,
                vehicles, seconds, request_operators ? "on" : "off");
    PdpRoutePlan plan = buildRandomInstance(requests, vehicles, /*seed=*/2);

    plan.rebuildUnassignedPool();
    std::printf("  initial (all unassigned): %s\n", ScoreCalculator::score(plan).str().c_str());

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = 2;
    // The control run keeps the engine's stop-granular moves only.
    config.pair_moves = request_operators;
    config.pair_exchange_moves = request_operators;
    config.pair_ruin = request_operators;
    config.request_ruin_recreate = request_operators;
    config.bundle_moves = request_operators;
    config.on_new_best = [](std::int64_t step, const Score& s) {
        std::printf("    new best @step %-8lld %s\n", static_cast<long long>(step),
                    s.str().c_str());
    };
    Solver(config).solve(plan);

    std::printf("  final score: %s  (feasible=%s)\n", plan.score.str().c_str(),
                plan.score.feasible() ? "true" : "false");
    for (const Vehicle& vehicle : plan.vehicles) {
        std::printf("    vehicle %s (cap %d): %zu stops\n", vehicle.id.c_str(), vehicle.capacity,
                    vehicle.nodeIndices.size());
    }
    int dropped = 0;
    for (const Delivery& delivery : plan.deliveries) {
        if (plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)].vehicleIndex < 0)
            ++dropped;
    }
    std::printf("    unrouted requests: %d / %zu\n", dropped, plan.deliveries.size());
    return 0;
}
