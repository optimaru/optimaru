// Semiconductor fab demo: a seeded random instance (photo / etch / diffusion
// tools, a batch furnace, re-entrant 6-step routes, a shared reticle), solved
// for N seconds (default 5). Prints the score and the constraint terms.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "fab/domain.hpp"
#include "fab/score_calculator.hpp"
#include "fab/solver.hpp"

using namespace fab;

namespace
{

SemiconductorSchedule buildDemo(int lotCount)
{
    std::mt19937_64 rng(7);
    auto pick = [&rng](int low, int high) { return low + static_cast<int>(rng() % static_cast<unsigned>(high - low + 1)); };
    SemiconductorSchedule s;
    const char *groups[] = {"PHOTO", "ETCH", "DIFF"};
    for (int g = 0; g < 3; ++g)
    {
        for (int m = 0; m < 2; ++m)
        {
            Machine machine;
            machine.id = std::string(groups[g]) + "-" + std::to_string(m + 1);
            machine.group = groups[g];
            if (g == 2)
            {
                machine.batchCapacity = 3;
                machine.minimumBatchSize = 2;
            }
            if (m == 0)
                machine.unavailable.push_back({20, 26});
            s.machines.push_back(machine);
        }
    }
    s.auxiliaryResources.push_back({"RETICLE", 1, {}});
    s.setupTimes.push_back({"PHOTO", "A", "B", 2});
    s.setupTimes.push_back({"PHOTO", "B", "A", 2});
    s.transportTimes.push_back({"PHOTO", "ETCH", 1});
    s.transportTimes.push_back({"ETCH", "DIFF", 2});
    for (int l = 0; l < lotCount; ++l)
    {
        Lot lot;
        lot.id = "LOT-" + std::to_string(l + 1);
        lot.product = l % 2 == 0 ? "A" : "B";
        lot.releaseTime = pick(0, 10);
        lot.dueTime = pick(60, 120);
        lot.priority = pick(1, 3);
        for (int step = 0; step < 6; ++step)
        {
            const int g = step % 3;
            Operation op;
            op.id = "S" + std::to_string(step + 1);
            op.family = g == 2 ? "DIFF" : lot.product;
            const int duration = g == 2 ? 8 : pick(3, 7);
            op.eligibleMachines.push_back({2 * g, duration, 0});
            op.eligibleMachines.push_back({2 * g + 1, g == 2 ? duration : duration + 1, pick(0, 2)});
            if (g == 0)
                op.requiredResources.push_back(0);
            if (g == 1)
                op.maxQueueTime = 30;
            lot.operations.push_back(op);
        }
        s.lots.push_back(lot);
    }
    return s;
}

}  // namespace

int main(int argc, char **argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 5.0;
    SemiconductorSchedule schedule = buildDemo(12);
    ScoreCalculator::prepare(schedule);
    Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
    config.randomSeed = 1;
    Solver(config).solve(schedule);
    std::printf("score %s, makespan %lld, weighted tardiness %lld, cycle time %lld\n",
                schedule.score.toString().c_str(), static_cast<long long>(schedule.makespan),
                static_cast<long long>(schedule.totalWeightedTardiness),
                static_cast<long long>(schedule.totalCycleTime));
    const RawTotals raw = ScoreCalculator::rawCosts(schedule);
    for (int t = 0; t < kTermCount; ++t)
        std::printf("  term %2d: %lld\n", t, static_cast<long long>(raw[static_cast<std::size_t>(t)]));
    return schedule.score.hard == 0 ? 0 : 1;
}
