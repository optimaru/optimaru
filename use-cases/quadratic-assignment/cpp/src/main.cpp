// Quadratic-assignment demo: the web demo's six-site plant layout (with a
// pinned Office, a forbidden Assembly/Annex pair and location capacities),
// then a random 25-facility instance. Solves each for N seconds (default 2)
// and asserts a feasible bijection.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"
#include "qap/solver.hpp"

using namespace qap;

namespace
{

QuadraticAssignmentProblem plantLayout()
{
    QuadraticAssignmentProblem problem;
    const std::vector<std::string> facilities{"Assembly", "Warehouse", "Paint", "Inspection", "Packing", "Office"};
    const std::vector<std::int64_t> demands{8, 7, 6, 4, 5, 2};
    const std::vector<std::string> locations{"North", "East", "South", "West", "Center", "Annex"};
    const std::vector<std::int64_t> capacities{8, 8, 8, 8, 8, 2};
    for (std::size_t i = 0; i < facilities.size(); ++i)
        problem.facilities.push_back({facilities[i], demands[i], -1});
    for (std::size_t i = 0; i < locations.size(); ++i)
        problem.locations.push_back({locations[i], capacities[i]});
    problem.flows = {{0, 8, 5, 2, 7, 1}, {8, 0, 3, 4, 9, 2}, {5, 3, 0, 8, 4, 1},
                     {2, 4, 8, 0, 6, 3}, {7, 9, 4, 6, 0, 2}, {1, 2, 1, 3, 2, 0}};
    problem.distances = {{0, 2, 5, 4, 2, 6}, {2, 0, 3, 5, 1, 4}, {5, 3, 0, 2, 3, 5},
                         {4, 5, 2, 0, 4, 3}, {2, 1, 3, 4, 0, 3}, {6, 4, 5, 3, 3, 0}};
    problem.fixedAssignments.push_back({5, 5});      // Annex holds Office
    problem.forbiddenAssignments.push_back({0, 5});  // Assembly never at Annex
    return problem;
}

QuadraticAssignmentProblem randomInstance(int n, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> value(0, 9);
    QuadraticAssignmentProblem problem;
    for (int i = 0; i < n; ++i)
    {
        problem.facilities.push_back({"F" + std::to_string(i), std::nullopt, -1});
        problem.locations.push_back({"L" + std::to_string(i), std::nullopt});
    }
    problem.flows.assign(n, std::vector<std::int64_t>(n, 0));
    problem.distances.assign(n, std::vector<std::int64_t>(n, 0));
    for (int a = 0; a < n; ++a)
    {
        for (int b = a + 1; b < n; ++b)
        {
            problem.flows[a][b] = problem.flows[b][a] = value(rng);
            problem.distances[a][b] = problem.distances[b][a] = 1 + value(rng);
        }
    }
    return problem;
}

bool run(const char *title, QuadraticAssignmentProblem problem, int seconds)
{
    ScoreCalculator::calculateScore(problem);
    std::printf("== %s (n=%d, %d s) ==\n", title, problem.facilityCount(), seconds);
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    { std::printf("    new best @step %-8lld %s\n", static_cast<long long>(step), score.toString().c_str()); };
    Solver(config).solve(problem);
    std::printf("  final score: %s  total cost %lld  bijection=%s\n",
                problem.score.toString().c_str(),
                static_cast<long long>(problem.totalCost()),
                problem.isBijection() ? "true" : "false");
    const std::vector<int> assignment = problem.assignment();
    for (int l = 0; l < problem.locationCount() && l < 8; ++l)
        std::printf("    %-8s <- %s\n", problem.locations[l].id.c_str(), problem.facilities[assignment[l]].id.c_str());
    std::printf("\n");
    return problem.isBijection() && problem.score.isFeasible();
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 2;
    bool ok = run("plant layout", plantLayout(), seconds);
    ok = run("random symmetric instance", randomInstance(25, 7), seconds) && ok;
    std::printf("%s\n", ok ? "feasible bijections: OK" : "FAIL");
    return ok ? 0 : 1;
}
