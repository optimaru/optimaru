// Facility-location demo, reproducing the Java quickstart's
// DemoDataGenerator instance scale: 30 facilities / 60 consumers spread
// uniformly over a London bounding box, total capacity 4500 vs total demand
// 900, setup cost ~ N(50000, 10000^2). Solves for N seconds (default 10) and
// asserts a feasible (0hard) result.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "fl/solver.hpp"

using namespace fl;

namespace
{

// DemoDataBuilder parameters (bootstrap/DemoDataGenerator.java).
constexpr std::int64_t kTotalCapacity = 4500;
constexpr std::int64_t kTotalDemand = 900;
constexpr int kFacilityCount = 30;
constexpr int kConsumerCount = 60;
constexpr std::int64_t kAverageSetupCost = 50000;
constexpr std::int64_t kSetupCostStdDev = 10000;
const Location kSouthWest{51.44, -0.16};
const Location kNorthEast{51.56, -0.01};

// Same structure as DemoDataBuilder.build(): uniform locations inside the
// bounding box, equal capacity/demand shares, Gaussian setup costs. C++'s
// RNG streams differ from java.util.Random, so the instance matches the
// Java demo in scale and distribution, not bit-for-bit.
FacilityLocationProblem buildDemoProblem()
{
    std::mt19937_64 rng(0);
    std::uniform_real_distribution<double> latitudes(kSouthWest.latitude,
                                                     kNorthEast.latitude);
    std::uniform_real_distribution<double> longitudes(kSouthWest.longitude,
                                                      kNorthEast.longitude);
    std::normal_distribution<double> gaussian(0.0, 1.0);

    FacilityLocationProblem problem;
    problem.southWestCorner = kSouthWest;
    problem.northEastCorner = kNorthEast;

    long sequence = 0;
    for (int f = 0; f < kFacilityCount; ++f)
    {
        Facility facility;
        facility.id = std::to_string(++sequence);
        facility.location = {latitudes(rng), longitudes(rng)};
        facility.setupCost
            = kAverageSetupCost
              + static_cast<std::int64_t>(kSetupCostStdDev * gaussian(rng));
        facility.capacity = kTotalCapacity / kFacilityCount;
        problem.facilities.push_back(facility);
    }
    for (int c = 0; c < kConsumerCount; ++c)
    {
        Consumer consumer;
        consumer.id = std::to_string(++sequence);
        consumer.location = {latitudes(rng), longitudes(rng)};
        consumer.demand = kTotalDemand / kConsumerCount;
        problem.consumers.push_back(consumer);
    }
    return problem;
}

void printSummary(const FacilityLocationProblem &problem)
{
    // Per-facility usage (only used facilities are listed).
    std::vector<std::int64_t> usedDemand(problem.facilities.size(), 0);
    std::vector<int> consumerCnt(problem.facilities.size(), 0);
    for (const Consumer &consumer : problem.consumers)
    {
        if (consumer.isAssigned())
        {
            usedDemand[consumer.facility] += consumer.demand;
            ++consumerCnt[consumer.facility];
        }
    }
    std::printf("  facilities used (%d/%d):\n",
                problem.usedFacilityCount(),
                problem.facilityCount());
    for (std::size_t f = 0; f < problem.facilities.size(); ++f)
    {
        if (consumerCnt[f] == 0)
            continue;
        const Facility &facility = problem.facilities[f];
        std::printf("    facility %-3s setup $%-6lld capacity %lld/%lld  consumers %d\n",
                    facility.id.c_str(),
                    static_cast<long long>(facility.setupCost),
                    static_cast<long long>(usedDemand[f]),
                    static_cast<long long>(facility.capacity),
                    consumerCnt[f]);
    }
    std::printf("\n  total setup cost: $%lld (all facilities would cost $%lld)\n",
                static_cast<long long>(problem.totalCost()),
                static_cast<long long>(problem.potentialCost()));
    std::printf("  total travel distance: %lld km\n",
                static_cast<long long>(problem.totalDistanceMeters() / 1000));
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    FacilityLocationProblem problem = buildDemoProblem();
    std::printf("== facility location demo (%d facilities x capacity %lld, %d consumers x demand "
                "%lld, %d s) ==\n",
                problem.facilityCount(),
                static_cast<long long>(kTotalCapacity / kFacilityCount),
                problem.consumerCount(),
                static_cast<long long>(kTotalDemand / kConsumerCount),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(problem);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                problem.score.toString().c_str(),
                problem.score.isFeasible() ? "true" : "false");
    printSummary(problem);

    bool allAssigned = true;
    for (const Consumer &consumer : problem.consumers)
    {
        if (!consumer.isAssigned())
            allAssigned = false;
    }
    std::printf("\n  all consumers assigned: %s\n", allAssigned ? "OK" : "FAIL");

    if (problem.score.hard == 0 && allAssigned)
    {
        std::printf("  reached hard 0: OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld)\n",
                static_cast<long long>(problem.score.hard));
    return 1;
}
