// Assembly-line-balancing demo: a random SALBP-1 instance (a layered
// precedence graph, durations 1..20, cycle time 50) solved for N seconds
// (default 5). Reports the stations used against the trivial lower bound
// ceil(total duration / cycle time) and asserts a feasible (0hard) result.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "alb/domain.hpp"
#include "alb/solver.hpp"

using namespace alb;

namespace {

AssemblyLineBalancingProblem buildDemoProblem(int taskCount, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> durations(1, 20);
    AssemblyLineBalancingProblem problem;
    problem.cycleTime = 50;
    for (int t = 0; t < taskCount; ++t) {
        Task task;
        task.id = "T" + std::to_string(t + 1);
        task.duration = durations(rng);
        problem.tasks.push_back(task);
    }
    // Each task depends on up to two random earlier tasks: acyclic by
    // construction.
    for (int t = 1; t < taskCount; ++t) {
        const int arcs = static_cast<int>(rng() % 3);
        for (int k = 0; k < arcs; ++k)
            problem.precedences.push_back({static_cast<int>(rng() % t), t});
    }
    return problem;
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    AssemblyLineBalancingProblem problem = buildDemoProblem(100, 1);
    std::int64_t total = 0;
    for (const Task& task : problem.tasks) total += task.duration;
    const std::int64_t lowerBound = (total + problem.cycleTime - 1) / problem.cycleTime;

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = 42;
    Solver solver(config);
    solver.solve(problem);

    std::printf("tasks %d, cycle time %lld, total duration %lld\n", problem.taskCount(),
                static_cast<long long>(problem.cycleTime), static_cast<long long>(total));
    std::printf("score %s, stations used %d (lower bound %lld)\n", problem.score.str().c_str(),
                problem.stationsUsed(), static_cast<long long>(lowerBound));
    if (problem.score.hard != 0) {
        std::printf("FAIL: infeasible result\n");
        return 1;
    }
    for (const Task& task : problem.tasks) {
        if (task.station < 0) {
            std::printf("FAIL: task %s unassigned\n", task.id.c_str());
            return 1;
        }
    }
    return 0;
}
