// Demo: the web app's max-cut sample (8 vertices, 14 weighted edges, A fixed
// to side 0, B and E together, C and F apart, exactly 4 vertices per side),
// solved for two seconds.
//
//   mc_demo [seconds]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mc/constraints.hpp"
#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"
#include "mc/solver.hpp"

using namespace mc;

namespace
{

MaxCutProblem webSample()
{
    MaxCutProblem problem;
    problem.vertexIds = {"A", "B", "C", "D", "E", "F", "G", "H"};
    enum { A, B, C, D, E, F, G, H };
    problem.edges = {{A, B, 7}, {A, C, 4}, {A, H, 6}, {B, D, 8}, {B, E, 3}, {C, D, 5}, {C, F, 9},
                     {D, G, 6}, {E, F, 4}, {E, H, 7}, {F, G, 5}, {G, H, 8}, {B, G, 2}, {C, E, 3}};
    problem.fixedSide.assign(8, kUnassigned);
    problem.fixedSide[A] = 0;
    problem.samePartitionPairs = {{B, E}};
    problem.differentPartitionPairs = {{C, F}};
    problem.minPartitionSize = 4;
    problem.maxPartitionSize = 4;
    return problem;
}

}  // namespace

int main(int argc, char **argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    MaxCutProblem problem = webSample();

    Solver::Config config;
    config.time_limit = std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    config.seed = 1;
    Solver solver(config);
    solver.solve(problem);

    std::printf("Score: %s (cut weight %lld of %lld)\n", problem.score.str().c_str(),
                static_cast<long long>(problem.cutWeight()), static_cast<long long>(problem.totalEdgeWeight()));
    for (int s = 0; s < kSideCount; ++s)
    {
        std::printf("Side %d:", s);
        for (int v = 0; v < problem.vertexCount(); ++v)
        {
            if (problem.side[v] == s)
                std::printf(" %s", problem.vertexIds[v].c_str());
        }
        std::printf("\n");
    }

    // The same solution explained per constraint by the default assembly.
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(problem);
    calculator.score();
    for (const optimaru::ConstraintTotal &total : calculator.constraint_totals())
    {
        std::printf("  %-28s %-4s %g\n", total.name.c_str(), optimaru::level_name(total.level), total.cost);
    }
    return 0;
}
