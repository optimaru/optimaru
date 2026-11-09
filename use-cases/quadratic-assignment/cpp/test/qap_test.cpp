// Monolithic-path tests (ScoreCalculator, the incremental calculator and the
// Solver), engine test/ style: check() + a non-zero exit on any failure.
//   (a) hand-calculated scores: the 0.1.x 2x2 / side-constraint / 3x3 cases
//   (b) each constraint's raw total, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation), plus the edge cases
//       (diagonal flow, repeated forbidden rows, one facility pinned twice,
//       missing demand / capacity, negative costs, an empty pinned location)
//   (c) prepare() validation
//   (d) incremental == full recompute, score and per-constraint raw totals,
//       across random swaps and unassign / reassign sequences driven through
//       the engine's Editor; every constraint is non-zero on some step
//   (e) the solver: zero budget keeps the start (identity or seed), warm
//       start rules, n = 0 / 1, bijection after single / parallel / ScoreChecks::Full
//       solves, brute-force optimum on small instances
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/editor.hpp"
#include "qap/domain.hpp"
#include "qap/incremental_calculator.hpp"
#include "qap/score_calculator.hpp"
#include "qap/solver.hpp"
#include "qap_fixtures.hpp"

using namespace qap;
using namespace qap_test;

namespace
{

using Raw = ScoreCalculator::RawCosts;

bool rawIs(const Raw &raw, std::int64_t pinned, std::int64_t forbidden, std::int64_t capacity,
           std::int64_t direct, std::int64_t flow)
{
    return raw.pinnedAssignmentsCost == pinned && raw.forbiddenAssignmentsCost == forbidden
           && raw.locationCapacityCost == capacity && raw.assignmentCost == direct && raw.flowDistanceCost == flow;
}

// The five raw totals in catalog order.
std::vector<std::int64_t> perConstraint(const Raw &raw)
{
    return {raw.pinnedAssignmentsCost, raw.forbiddenAssignmentsCost, raw.locationCapacityCost, raw.assignmentCost,
            raw.flowDistanceCost};
}

const char *const kConstraintNames[] = {"pinnedAssignmentsCost", "forbiddenAssignmentsCost", "locationCapacityCost",
                                        "assignmentCost", "flowDistanceCost"};

// -------------------------------------------------------------------------
// (a) Hand-calculated scores.
// -------------------------------------------------------------------------
void verifyHandScores()
{
    std::printf("== (a) Hand-calculated scores ==\n");
    {
        // identity: D[0][1] * F[0][1] + D[1][0] * F[1][0] = 4*3 + 5*2 = 22
        QuadraticAssignmentProblem p = twoFacilities();
        identity(p);
        check(!p.score, "a problem never scored has no score");
        check(sameScore(ScoreCalculator::score(p), Score{0, -22.0}), "2x2 identity scores 0hard/-22soft");
        check(p.score && sameScore(*p.score, Score{0, -22.0}), "calculateScore stores problem.score");
        check(p.totalCost() == 22 && p.flowDistanceCost() == 22 && p.assignmentCost() == 0, "2x2 total cost 22");
        // swapped: D[0][1] * F[1][0] + D[1][0] * F[0][1] = 4*2 + 5*3 = 23
        place(p, {1, 0});
        check(sameScore(ScoreCalculator::score(p), Score{0, -23.0}), "2x2 swapped scores 0hard/-23soft");
    }
    {
        // L1<-F1, L2<-F2: pin L1->F2 broken (1), F2 at L2 forbidden (1),
        // L1 carries 5 on 3 (2), direct 7 + 8 = 15, flow 22.
        QuadraticAssignmentProblem p = sideInstance();
        identity(p);
        check(rawIs(ScoreCalculator::rawCosts(p), 1, 1, 2, 15, 22), "side instance, identity: raw 1/1/2/15/22");
        check(sameScore(ScoreCalculator::score(p), Score{-4, -37.0}), "side instance, identity: -4hard/-37soft");
        // L1<-F2, L2<-F1: every side constraint kept, direct 2 + 1 = 3, flow 23.
        place(p, {1, 0});
        check(rawIs(ScoreCalculator::rawCosts(p), 0, 0, 0, 3, 23), "side instance, swapped: raw 0/0/0/3/23");
        check(sameScore(ScoreCalculator::score(p), Score{0, -26.0}), "side instance, swapped: 0hard/-26soft");
    }
    {
        // The 0.1.x seed {L3: F1, L1: F2, L2: F3}: 2 * (3*4 + 1*2 + 2*1) = 32.
        QuadraticAssignmentProblem p = threeFacilities();
        place(p, {1, 2, 0});
        check(p.totalCost() == 32, "3x3 seed total cost 32");
        check(sameScore(ScoreCalculator::score(p), Score{0, -32.0}), "3x3 seed scores 0hard/-32soft");
        check(p.assignment() == std::vector<int>({1, 2, 0}) && p.isBijection(), "assignment() per location");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Each constraint: a positive and a zero case, plus the edge cases.
// -------------------------------------------------------------------------
void verifyConstraintsByHand()
{
    std::printf("== (b) Raw cost per constraint, by hand ==\n");
    // pinnedAssignmentsCost
    {
        QuadraticAssignmentProblem p = emptyInstance(3);
        p.fixedAssignments.push_back({2, 0});  // L3 must hold F1
        identity(p);
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 1, "pinned: F1 at L1 instead of L3 -> 1");
        place(p, {1, 2, 0});
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 0, "pinned: F1 at L3 -> 0");
        // An empty pinned location is a broken pin (partial state).
        place(p, {1, 2, -1});
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 1, "pinned: L3 empty -> 1");
    }
    {
        // One facility pinned to two locations: at least one is broken.
        QuadraticAssignmentProblem p = emptyInstance(3);
        p.fixedAssignments.push_back({0, 0});
        p.fixedAssignments.push_back({1, 0});
        identity(p);
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 1, "pinned twice: F1 at L1 -> exactly 1");
        place(p, {1, 0, 2});
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 1, "pinned twice: F1 at L2 -> exactly 1");
        place(p, {1, 2, 0});
        check(ScoreCalculator::rawCosts(p).pinnedAssignmentsCost == 2, "pinned twice: F1 at L3 -> 2");
    }
    // forbiddenAssignmentsCost
    {
        QuadraticAssignmentProblem p = emptyInstance(3);
        p.forbiddenAssignments.push_back({0, 1});  // F1 never at L2
        p.forbiddenAssignments.push_back({0, 1});  // ... listed twice: counted once
        p.forbiddenAssignments.push_back({2, 2});  // F3 never at L3
        place(p, {1, 0, 2});
        check(ScoreCalculator::rawCosts(p).forbiddenAssignmentsCost == 2,
              "forbidden: F1 at L2 (row listed twice) + F3 at L3 -> 2");
        place(p, {0, 2, 1});
        check(ScoreCalculator::rawCosts(p).forbiddenAssignmentsCost == 0, "forbidden: no forbidden pair used -> 0");
    }
    // locationCapacityCost
    {
        QuadraticAssignmentProblem p = emptyInstance(3);
        p.facilities[0].demand = 7;
        p.facilities[1].demand = 0;
        // F3 has no demand entry: demand 0.
        p.locations[0].capacity = 4;
        p.locations[1].capacity = 0;
        // L3 has no capacity: no limit.
        identity(p);
        check(ScoreCalculator::rawCosts(p).locationCapacityCost == 3, "capacity: 7 on 4 at L1 -> 3");
        place(p, {1, 2, 0});
        check(ScoreCalculator::rawCosts(p).locationCapacityCost == 0,
              "capacity: demand 0 on 4, missing demand on 0, 7 on an uncapacitated location -> 0");
        place(p, {2, 0, 1});
        check(ScoreCalculator::rawCosts(p).locationCapacityCost == 7, "capacity: 7 on capacity 0 -> 7");
    }
    // assignmentCost
    {
        QuadraticAssignmentProblem p = emptyInstance(2);
        p.assignmentCosts = std::vector<std::vector<std::int64_t>>{{4, -6}, {2, 0}};
        identity(p);
        check(ScoreCalculator::rawCosts(p).assignmentCost == 4, "direct: 4 + 0 -> 4");
        place(p, {1, 0});
        check(ScoreCalculator::rawCosts(p).assignmentCost == -4, "direct: -6 + 2 -> -4 (a negative raw cost)");
        check(sameScore(ScoreCalculator::score(p), Score{0, 4.0}), "a negative raw cost scores a soft bonus");
        QuadraticAssignmentProblem none = emptyInstance(2);
        identity(none);
        check(ScoreCalculator::rawCosts(none).assignmentCost == 0, "direct: no matrix -> 0");
    }
    // flowDistanceCost
    {
        QuadraticAssignmentProblem p = emptyInstance(2);
        p.flows[0][0] = 5;  // the diagonal term flows[a][a] * distances[l][l]
        p.distances[1][1] = 3;
        place(p, {1, 0});
        check(ScoreCalculator::rawCosts(p).flowDistanceCost == 15, "flow: diagonal 5 * 3 counted -> 15");
        identity(p);
        check(ScoreCalculator::rawCosts(p).flowDistanceCost == 0, "flow: diagonal on distance 0 -> 0");
        // An unassigned facility contributes nothing.
        QuadraticAssignmentProblem q = twoFacilities();
        place(q, {0, -1});
        check(ScoreCalculator::rawCosts(q).flowDistanceCost == 0, "flow: one facility unassigned -> 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) prepare() validation.
// -------------------------------------------------------------------------
bool throwsOn(const std::function<void(QuadraticAssignmentProblem &)> &edit)
{
    QuadraticAssignmentProblem p = sideInstance();
    edit(p);
    try
    {
        ScoreCalculator::prepare(p);
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    catch (const std::overflow_error &)
    {
        return true;
    }
    return false;
}

void verifyValidation()
{
    std::printf("== (c) prepare() validation ==\n");
    check(!throwsOn([](QuadraticAssignmentProblem &) {}), "the side instance is valid");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.locations.pop_back(); }), "facility / location counts differ");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.flows[1].push_back(0); }), "flows not square");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.distances.pop_back(); }), "distances mis-sized");
    check(throwsOn([](QuadraticAssignmentProblem &p) { (*p.assignmentCosts)[0].pop_back(); }),
          "assignment_costs mis-sized");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.fixedAssignments.push_back({0, 0}); }),
          "a location fixed twice");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.fixedAssignments.push_back({1, 5}); }),
          "fixed facility out of range");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.forbiddenAssignments.push_back({0, -1}); }),
          "forbidden location out of range");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.facilities[0].demand = -1; }), "negative demand");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.locations[1].capacity = -1; }), "negative capacity");
    check(throwsOn([](QuadraticAssignmentProblem &p) { p.facilities[0].location = 2; }), "location index out of range");
    check(throwsOn(
              [](QuadraticAssignmentProblem &p)
              {
                  p.flows[0][1] = std::numeric_limits<std::int64_t>::max() / 2;
                  p.distances[0][1] = 9;
              }),
          "objective magnitudes beyond int64");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Incremental == full recompute through the Editor.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (d) Incremental == full recompute ==\n");
    int mismatches = 0;
    int rawMismatches = 0;
    int checks = 0;
    // Steps on which each constraint's raw total was non-zero: every
    // constraint must take part in the walk, or its incremental path goes
    // unchecked.
    std::vector<int> nonZero(5, 0);
    for (int round = 0; round < 4; ++round)
    {
        QuadraticAssignmentProblem p = randomInstance(12, 31 + round);
        std::mt19937_64 rng(100 + round);
        place(p, randomPermutation(12, rng));
        QuadraticAssignmentIncrementalScoreCalculator calculator;
        optimaru::Editor<QuadraticAssignmentProblem> editor(p, calculator);
        std::uniform_int_distribution<int> facility(0, 11);
        std::uniform_int_distribution<int> kind(0, 9);
        for (int step = 0; step < 1250; ++step)
        {
            const int a = facility(rng);
            const int b = facility(rng);
            const int what = kind(rng);
            if (what < 7)
            {
                // A swap (the solver's only move), batched like the engine does.
                const int la = p.facilities[a].location;
                const int lb = p.facilities[b].location;
                editor.begin_batch();
                editor.assign(a, lb);
                editor.assign(b, la);
                editor.end_batch();
            }
            else if (what < 9)
            {
                // Unassign / reassign (partial states a caller may write).
                const int target = p.facilities[a].location < 0 ? facility(rng) : -1;
                editor.assign(a, target);
            }
            else
            {
                editor.assign(a, facility(rng));  // a change move (two on one location)
            }
            const Score incremental = editor.score();
            QuadraticAssignmentProblem copy = p;
            const Score full = ScoreCalculator::score(copy);
            const std::vector<std::int64_t> live = perConstraint(calculator.rawCosts());
            const std::vector<std::int64_t> reset = perConstraint(ScoreCalculator::rawCosts(copy));
            ++checks;
            if (!sameScore(incremental, full) && ++mismatches <= 3)
            {
                std::printf("  MISMATCH round %d step %d: incremental=%s full=%s\n", round, step,
                            incremental.str().c_str(), full.str().c_str());
            }
            for (std::size_t c = 0; c < reset.size(); ++c)
            {
                if (live[c] != reset[c] && ++rawMismatches <= 3)
                {
                    std::printf("  RAW MISMATCH round %d step %d %s: incremental=%lld full=%lld\n", round, step,
                                kConstraintNames[c], static_cast<long long>(live[c]),
                                static_cast<long long>(reset[c]));
                }
                if (reset[c] != 0)
                    ++nonZero[c];
            }
        }
    }
    check(mismatches == 0, std::to_string(checks) + " random swaps / unassigns / changes: incremental == full");
    check(rawMismatches == 0, "per-constraint raw totals after every move: incremental == full");
    for (std::size_t c = 0; c < nonZero.size(); ++c)
    {
        check(nonZero[c] > 0,
              std::string(kConstraintNames[c]) + " non-zero on " + std::to_string(nonZero[c]) + " walk steps");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) The solver.
// -------------------------------------------------------------------------
Solver::Config budget(std::int64_t steps, std::uint64_t seed = 1)
{
    Solver::Config config;
    config.time_limit = std::chrono::seconds(60);
    config.step_limit = steps;
    config.seed = seed;
    return config;
}

Solver::Config zeroBudget()
{
    Solver::Config config;
    config.time_limit = std::chrono::milliseconds(0);
    return config;
}

template <typename Exception>
bool throws(const std::function<void()> &run)
{
    try
    {
        run();
    }
    catch (const Exception &)
    {
        return true;
    }
    return false;
}

Score bruteForceOptimum(QuadraticAssignmentProblem problem)
{
    const int n = problem.facilityCount();
    std::vector<int> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    Score best{std::numeric_limits<std::int64_t>::min(), -std::numeric_limits<double>::infinity()};
    do
    {
        place(problem, perm);
        const Score score = ScoreCalculator::score(problem);
        if (score > best)
            best = score;
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

void verifySolver()
{
    std::printf("== (e) Solver ==\n");
    {
        // Zero budget: the identity, whatever the input assignment said.
        QuadraticAssignmentProblem p = threeFacilities();
        place(p, {2, 0, 1});
        Solver(zeroBudget()).solve(p);
        check(p.assignment() == std::vector<int>({0, 1, 2}), "zero budget, no seed: the identity");
        // Zero budget with a seed: the seed, unchanged, and the seed is consumed.
        QuadraticAssignmentProblem s = threeFacilities();
        place(s, {1, 2, 0});
        s.warmStart = true;
        Solver(zeroBudget()).solve(s);
        check(s.assignment() == std::vector<int>({1, 2, 0}) && s.totalCost() == 32 && sameScore(*s.score, {0, -32.0}),
              "zero budget, seeded: the seed comes back with its score");
        check(!s.warmStart, "the seed is used once (warmStart reset)");
        // A seed that breaks a pin survives a zero-budget solve with hard -1.
        QuadraticAssignmentProblem b = threeFacilities();
        b.fixedAssignments.push_back({0, 0});
        place(b, {1, 0, 2});
        b.warmStart = true;
        Solver(zeroBudget()).solve(b);
        check(b.assignment() == std::vector<int>({1, 0, 2}) && b.score->hard == -1,
              "zero budget, seed breaking a pin: kept, hard -1");
    }
    {
        QuadraticAssignmentProblem p = threeFacilities();
        p.facilities[0].location = 0;
        p.facilities[1].location = 0;  // two facilities on L1
        p.facilities[2].location = 1;  // ... and L3 empty
        p.warmStart = true;
        check(throws<std::invalid_argument>([&] { Solver(budget(10)).solve(p); }),
              "a non-bijective warm start throws");
        QuadraticAssignmentProblem partial = threeFacilities();
        place(partial, {0, 1, -1});
        partial.warmStart = true;
        check(throws<std::invalid_argument>([&] { Solver(budget(10)).solve(partial); }), "a partial warm start throws");
        Solver::Config bad = budget(10);
        bad.parallelSolverCount = 0;
        QuadraticAssignmentProblem q = threeFacilities();
        check(throws<std::invalid_argument>([&] { Solver(bad).solve(q); }), "parallelSolverCount 0 throws");
    }
    {
        QuadraticAssignmentProblem empty = emptyInstance(0);
        Solver(budget(100)).solve(empty);
        check(sameScore(*empty.score, {0, 0.0}) && empty.isBijection(), "n = 0 solves to 0hard/0soft");
        QuadraticAssignmentProblem one = emptyInstance(1);
        one.flows = {{3}};
        one.distances = {{2}};
        one.fixedAssignments.push_back({0, 0});
        Solver(budget(100)).solve(one);
        check(one.assignment() == std::vector<int>({0}) && sameScore(*one.score, {0, -6.0}),
              "n = 1: the only assignment, diagonal 3 * 2");
    }
    {
        QuadraticAssignmentProblem p = randomInstance(15, 5);
        Solver(budget(3000, 3)).solve(p);
        QuadraticAssignmentProblem copy = p;
        check(p.isBijection() && sameScore(*p.score, ScoreCalculator::score(copy)),
              "single solve: a bijection, score = full recompute");
        QuadraticAssignmentProblem q = randomInstance(15, 5);
        Solver::Config parallel = budget(3000, 3);
        parallel.parallelSolverCount = 4;
        Solver(parallel).solve(q);
        check(q.isBijection(), "4 parallel starts: a bijection");
        QuadraticAssignmentProblem f = randomInstance(10, 8);
        Solver::Config full = budget(1500, 9);
        full.score_checks = optimaru::ScoreChecks::Full;
        bool ok = true;
        try
        {
            Solver(full).solve(f);
        }
        catch (const std::exception &error)
        {
            std::printf("  %s\n", error.what());
            ok = false;
        }
        check(ok && f.isBijection(), "FULL_ASSERT solve: no score corruption, a bijection");
        // The entity-tabu acceptor keeps the permutation too.
        QuadraticAssignmentProblem t = randomInstance(10, 12);
        Solver::Config tabu = budget(2000, 4);
        tabu.rule = optimaru::AcceptanceRule::EntityTabu;
        tabu.score_checks = optimaru::ScoreChecks::Full;
        Solver(tabu).solve(t);
        check(t.isBijection(), "ENTITY_TABU FULL_ASSERT solve: a bijection");
    }
    {
        int reached = 0;
        const int instances = 8;
        for (int k = 0; k < instances; ++k)
        {
            const int n = 4 + k % 4;
            QuadraticAssignmentProblem p = randomInstance(n, 700 + k);
            const Score optimum = bruteForceOptimum(p);
            Solver(budget(20000, 11 + k)).solve(p);
            if (sameScore(*p.score, optimum))
                ++reached;
            else
                std::printf("  n=%d: solver %s, optimum %s\n", n, p.score->str().c_str(),
                            optimum.str().c_str());
        }
        check(reached == instances, std::to_string(reached) + "/" + std::to_string(instances)
                                        + " small instances (n = 4..7) reach the brute-force optimum");
    }
    {
        // on_new_best fires and the seeded solve is reproducible.
        int calls = 0;
        Solver::Config config = budget(2000, 21);
        config.on_new_best = [&calls](std::int64_t, const Score &) { ++calls; };
        QuadraticAssignmentProblem a = randomInstance(12, 77);
        Solver(config).solve(a);
        QuadraticAssignmentProblem b = randomInstance(12, 77);
        Solver(budget(2000, 21)).solve(b);
        check(calls > 0, "onNewBest is called");
        check(a.assignment() == b.assignment(), "a seeded step-limited solve is reproducible");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyHandScores();
    verifyConstraintsByHand();
    verifyValidation();
    verifyIncremental();
    verifySolver();
    if (failures != 0)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
