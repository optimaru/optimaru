// Shared test fixtures for qap_test and qap_plugin_test: the check() harness,
// the hand-calculated instances and a seeded random instance generator.
#ifndef QAP_TEST_FIXTURES_HPP
#define QAP_TEST_FIXTURES_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "qap/domain.hpp"

namespace qap_test
{

using namespace qap;

inline int failures = 0;

inline void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

inline bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

// n facilities F1..Fn and locations L1..Ln, no side data, nothing placed.
inline QuadraticAssignmentProblem emptyInstance(int n)
{
    QuadraticAssignmentProblem problem;
    for (int i = 0; i < n; ++i)
    {
        problem.facilities.push_back({"F" + std::to_string(i + 1), std::nullopt, -1});
        problem.locations.push_back({"L" + std::to_string(i + 1), std::nullopt});
    }
    problem.flows.assign(n, std::vector<std::int64_t>(n, 0));
    problem.distances.assign(n, std::vector<std::int64_t>(n, 0));
    return problem;
}

// assignment[l] = the facility standing at location l (-1 = empty); every
// facility not named is unassigned. Resets `prepared` only for the variable
// (facts unchanged), as the bindings' set_assignment does.
inline void place(QuadraticAssignmentProblem &problem, const std::vector<int> &assignment)
{
    for (Facility &facility : problem.facilities)
        facility.location = -1;
    for (std::size_t l = 0; l < assignment.size(); ++l)
    {
        if (assignment[l] >= 0)
            problem.facilities[assignment[l]].location = static_cast<int>(l);
    }
}

inline void identity(QuadraticAssignmentProblem &problem)
{
    for (int f = 0; f < problem.facilityCount(); ++f)
        problem.facilities[f].location = f;
}

// The 2x2 hand-calculation instance:
// flows [[0,3],[2,0]], distances [[0,4],[5,0]]; identity costs 4*3 + 5*2 = 22.
inline QuadraticAssignmentProblem twoFacilities()
{
    QuadraticAssignmentProblem problem = emptyInstance(2);
    problem.flows = {{0, 3}, {2, 0}};
    problem.distances = {{0, 4}, {5, 0}};
    return problem;
}

// The side-constraint instance: the 2x2 flows and
// distances plus assignment costs [[7,1],[2,8]], L1 fixed to F2, (F2, L2)
// forbidden, demands F1=5 F2=2, capacities L1=3 L2=10.
inline QuadraticAssignmentProblem sideInstance()
{
    QuadraticAssignmentProblem problem = twoFacilities();
    problem.assignmentCosts = std::vector<std::vector<std::int64_t>>{{7, 1}, {2, 8}};
    problem.fixedAssignments.push_back({0, 1});
    problem.forbiddenAssignments.push_back({1, 1});
    problem.facilities[0].demand = 5;
    problem.facilities[1].demand = 2;
    problem.locations[0].capacity = 3;
    problem.locations[1].capacity = 10;
    return problem;
}

// The 3x3 initial-solution instance.
inline QuadraticAssignmentProblem threeFacilities()
{
    QuadraticAssignmentProblem problem = emptyInstance(3);
    problem.flows = {{0, 3, 1}, {3, 0, 2}, {1, 2, 0}};
    problem.distances = {{0, 1, 4}, {1, 0, 2}, {4, 2, 0}};
    return problem;
}

// A seeded random instance with every kind of side data: optional direct
// costs (some negative), fixed locations (sometimes two on one facility),
// forbidden rows (with repeats), partial demand / capacity maps, flows and
// distances including the diagonal and negative entries. `negatives` = 0
// keeps every number >= 0 (the CP-SAT-friendly regime).
inline QuadraticAssignmentProblem randomInstance(int n, std::uint64_t seed, bool negatives = true)
{
    std::mt19937_64 rng(seed);
    auto uniform = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    QuadraticAssignmentProblem problem = emptyInstance(n);
    const int low = negatives ? -3 : 0;
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            problem.flows[a][b] = uniform(low, 9);
            problem.distances[a][b] = uniform(low, 9);
        }
    }
    if (uniform(0, 3) != 0)
    {
        std::vector<std::vector<std::int64_t>> direct(n, std::vector<std::int64_t>(n, 0));
        for (auto &row : direct)
        {
            for (auto &value : row)
                value = uniform(negatives ? -5 : 0, 9);
        }
        problem.assignmentCosts = direct;
    }
    std::vector<int> locations(n);
    std::iota(locations.begin(), locations.end(), 0);
    std::shuffle(locations.begin(), locations.end(), rng);
    const int fixedCount = uniform(0, std::max(0, n / 3));
    for (int k = 0; k < fixedCount; ++k)
        problem.fixedAssignments.push_back({locations[k], uniform(0, n - 1)});
    const int forbiddenCount = uniform(0, n);
    for (int k = 0; k < forbiddenCount; ++k)
    {
        const ForbiddenAssignment row{uniform(0, n - 1), uniform(0, n - 1)};
        problem.forbiddenAssignments.push_back(row);
        if (uniform(0, 4) == 0)
            problem.forbiddenAssignments.push_back(row);  // a repeated row
    }
    for (Facility &facility : problem.facilities)
    {
        if (uniform(0, 2) != 0)
            facility.demand = uniform(0, 9);
    }
    for (Location &location : problem.locations)
    {
        if (uniform(0, 2) != 0)
            location.capacity = uniform(0, 9);
    }
    return problem;
}

// A uniformly random permutation: facility perm[l] at location l.
inline std::vector<int> randomPermutation(int n, std::mt19937_64 &rng)
{
    std::vector<int> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), rng);
    return perm;
}

}  // namespace qap_test

#endif  // QAP_TEST_FIXTURES_HPP
