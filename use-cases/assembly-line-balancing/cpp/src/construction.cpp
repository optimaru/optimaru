#include "alb/construction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace alb
{

std::vector<int> topologicalOrder(const AssemblyLineBalancingProblem &problem)
{
    const int n = problem.taskCount();
    std::vector<int> indegree(n, 0);
    std::vector<std::vector<int>> successors(n);
    for (const Precedence &arc : problem.precedences)
    {
        if (arc.before < 0 || arc.before >= n || arc.after < 0 || arc.after >= n)
            throw std::invalid_argument("precedence task index out of range");
        ++indegree[arc.after];
        successors[arc.before].push_back(arc.after);
    }
    auto byId = [&problem](int a, int b)
    {
        const std::string &left = problem.tasks[a].id;
        const std::string &right = problem.tasks[b].id;
        return left != right ? left < right : a < b;
    };

    std::vector<int> order;
    order.reserve(n);
    std::vector<int> level;
    for (int t = 0; t < n; ++t)
    {
        if (indegree[t] == 0)
            level.push_back(t);
    }
    while (static_cast<int>(order.size()) < n)
    {
        if (level.empty())
            throw std::invalid_argument("precedence graph must be acyclic");
        std::sort(level.begin(), level.end(), byId);
        order.insert(order.end(), level.begin(), level.end());
        // The next level: tasks whose last predecessor sat in this one.
        std::vector<int> next;
        for (int t : level)
        {
            for (int successor : successors[t])
            {
                if (--indegree[successor] == 0)
                    next.push_back(successor);
            }
        }
        level.swap(next);
    }
    return order;
}

void constructStations(AssemblyLineBalancingProblem &problem, const std::vector<int> &order)
{
    const int n = problem.taskCount();
    std::vector<char> seeded(n, 0);
    int top = -1;
    for (int t = 0; t < n; ++t)
    {
        if (problem.tasks[t].station >= 0)
        {
            seeded[t] = 1;
            top = std::max(top, problem.tasks[t].station);
        }
    }
    // The pointer never exceeds max(seed) + one station per unseeded task.
    std::vector<std::int64_t> load(static_cast<std::size_t>(std::max(top, 0)) + n + 2, 0);
    for (int t = 0; t < n; ++t)
    {
        if (seeded[t])
            load[problem.tasks[t].station] += problem.tasks[t].duration;
    }
    int station = 0;
    for (int t : order)
    {
        Task &task = problem.tasks[t];
        if (seeded[t])
        {
            station = std::max(station, task.station);
            continue;
        }
        while (load[station] != 0 && load[station] + task.duration > problem.cycleTime)
            ++station;
        task.station = station;
        load[station] += task.duration;
    }
    for (int t = 0; t < n; ++t)
    {
        Task &task = problem.tasks[t];
        if (task.fixedStation && !seeded[t])
            task.station = *task.fixedStation;
    }
}

}  // namespace alb
