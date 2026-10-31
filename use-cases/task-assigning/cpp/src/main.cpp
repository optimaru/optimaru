// Task-assigning demo: rebuilds the Java quickstart's DemoDataGenerator
// instance (4 task types, 4 customers, 8 employees, 28 tasks), solves for
// N seconds and asserts the result is feasible (hard == 0) with every task
// assigned.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "ta/demo_data.hpp"
#include "ta/domain.hpp"
#include "ta/solver.hpp"

using namespace ta;

namespace
{

void printSummary(const TaskAssigningSolution &solution)
{
    for (const Employee &employee : solution.employees)
    {
        Minutes endTime = 0;
        std::printf("  %-5s [%zu tasks]",
                    employee.fullName.c_str(),
                    employee.tasks.size());
        for (int taskIndex : employee.tasks)
        {
            const Task &task = solution.tasks[taskIndex];
            const Minutes end
                = task.startTime + solution.taskDuration(task, task.employee);
            std::printf("  %s-%d(%lld-%lld)",
                        solution.taskTypes[task.taskType].code.c_str(),
                        task.indexInTaskType,
                        static_cast<long long>(task.startTime),
                        static_cast<long long>(end));
            endTime = end;
        }
        std::printf("  -> end %lld\n", static_cast<long long>(endTime));
    }
    int unassigned = 0;
    for (const Task &task : solution.tasks)
    {
        if (task.employee < 0)
            ++unassigned;
    }
    std::printf("  unassigned tasks: %d\n\n", unassigned);
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    TaskAssigningSolution solution = demo::generateDemoData();
    std::printf("== task assigning demo (%zu task types, %zu customers, "
                "%zu employees, %zu tasks, %d s) ==\n",
                solution.taskTypes.size(),
                solution.customers.size(),
                solution.employees.size(),
                solution.tasks.size(),
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
    solver.solve(solution);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                solution.score.toString().c_str(),
                solution.score.isFeasible() ? "true" : "false");
    printSummary(solution);

    if (solution.score.hard == 0 && solution.score.soft0 == 0)
    {
        std::printf("  reached hard 0 + every task assigned: OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld, unassigned=%lld)\n",
                static_cast<long long>(solution.score.hard),
                static_cast<long long>(-solution.score.soft0));
    return 1;
}
