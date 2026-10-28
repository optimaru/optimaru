// Plugin-assembly (fp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 200 random list
//       layouts (unassigned jobs included)
//   (b) incremental score == full recompute across 1500 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) constraintTotals() explains exactly the reported score
//   (d) weight / isHard overrides
//   (e) a 2-second solve(schedule, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "fp/constraints.hpp"
#include "fp/demo_data.hpp"
#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "fp/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace fp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && a.medium == b.medium && a.soft == b.soft;
}

Score scoreWith(DecomposedCalculator &calculator, PackagingSchedule &s)
{
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.resetWorkingSolution(s);
    return fromEngineScore(calculator.calculateScore());
}

// Spreads every job over the lines at random, writing straight into the
// schedule's own lists. `poolRate` of them go to the unassigned pool list and
// another `poolRate` into NO list at all — the state a solution starts in,
// which the score treats as unassigned just the same.
void randomizeLists(PackagingSchedule &s, std::mt19937_64 &rng, double poolRate)
{
    const int jobCount = static_cast<int>(s.jobs.size());
    for (Line &line : s.lines)
        line.jobs.clear();
    s.unassignedJobs.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int j = 0; j < jobCount; ++j)
    {
        const double roll = unit(rng);
        if (roll < poolRate)
        {
            s.unassignedJobs.push_back(j);
            continue;
        }
        if (roll < 2 * poolRate)
            continue;  // in no list at all
        s.lines[rng() % s.lines.size()].jobs.push_back(j);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 30);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 200;
    for (int round = 0; round < rounds; ++round)
    {
        randomizeLists(s, rng, /*poolRate*/ 0.10);
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::calculateScore(s);
        if (!sameScore(plugin, monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random list layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector. The unassigned pool is one of
// the engine's lists here, so a "relocate to the pool" is an ordinary move.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 24);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<PackagingSchedule> director(s, calculator);

    using Descriptor = optimaru::SolutionDescriptor<PackagingSchedule>;
    const int listCount = Descriptor::listCount(s);
    const int elementCount = Descriptor::elementCount(s);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // Where each engine element currently sits (list, position is looked up).
    std::vector<int> listOf(elementCount, -1);
    for (int l = 0; l < listCount; ++l)
    {
        for (int p = 0; p < Descriptor::listSize(s, l); ++p)
            listOf[Descriptor::getElement(s, l, p)] = l;
    }
    auto indexWithin = [&](int list, int element)
    {
        for (int p = 0; p < Descriptor::listSize(s, list); ++p)
        {
            if (Descriptor::getElement(s, list, p) == element)
                return p;
        }
        return -1;
    };

    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step)
    {
        if (elementCount == 0)
            break;
        const int element = static_cast<int>(rng() % elementCount);
        if (unit(rng) < 0.8)
        {
            // Relocate to a random (list, position).
            const int from = listOf[element];
            if (from >= 0)
                director.removeListElement(from, indexWithin(from, element));
            const int to = static_cast<int>(rng() % listCount);
            const int position = static_cast<int>(
                rng() % static_cast<unsigned>(Descriptor::listSize(s, to) + 1));
            director.insertListElement(to, position, element);
            listOf[element] = to;
        }
        else
        {
            // Reverse a random segment (2-opt).
            const int list = static_cast<int>(rng() % listCount);
            const int size = Descriptor::listSize(s, list);
            if (size < 2)
                continue;
            int first = static_cast<int>(rng() % size);
            int last = static_cast<int>(rng() % size);
            if (first > last)
                std::swap(first, last);
            director.reverseListSegment(list, first, last);
        }

        const Score incremental = fromEngineScore(director.calculateScore());
        const Score full = ScoreCalculator::calculateScore(s);
        if (!sameScore(full, incremental))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) constraintTotals(): the per-constraint breakdown adds up, and each
// component isolates exactly its own term of the monolithic breakdown.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (c) Per-component totals vs the monolithic breakdown ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 24);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    randomizeLists(s, rng, /*poolRate*/ 0.15);

    const ConstraintBreakdown breakdown = ScoreCalculator::analyze(s);

    struct Case
    {
        const char *label;
        std::int64_t expected;
        Score (*score)(PackagingSchedule &);
    };

    {
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndDateTime>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == -breakdown.maxEndDateTime && score.medium == 0
                  && score.soft == 0,
              "maxEndDateTime alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<OperatorCleaningConflict>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == -breakdown.operatorCleaningConflict
                  && score.medium == 0 && score.soft == 0,
              "operatorCleaningConflict alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<IdealEndDateTime>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == -breakdown.idealEndDateTime
                  && score.soft == 0,
              "idealEndDateTime alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<MaximizeJobsAssigned>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == -breakdown.maximizeJobsAssigned
                  && score.soft == 0,
              "maximizeJobsAssigned alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeMakespan>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == 0
                  && score.soft == -breakdown.minimizeMakespan,
              "minimizeMakespan alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score score = scoreWith(calculator, s);
        double hard = 0.0;
        double soft = 0.0;
        for (const optimaru::ConstraintTotal &total :
             calculator.constraintTotals())
            (total.isHard ? hard : soft) += total.cost;
        const optimaru::HardSoftScore engineScore = toEngineScore(score);
        check(std::fabs(-hard - static_cast<double>(engineScore.hard)) < 1e-9
                  && std::fabs(-soft - engineScore.soft) < 1e-3,
              "per-constraint totals sum to the reported score");
        check(calculator.constraintTotals().size() == 5,
              "one total per assembled component");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 12);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(7);
    randomizeLists(s, rng, /*poolRate*/ 0.25);
    const ConstraintBreakdown breakdown = ScoreCalculator::analyze(s);

    {
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndDateTime>(3.0);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -3 * breakdown.maxEndDateTime,
              "maxEndDateTime re-weighted to 3");
    }
    {
        // Demoted to soft at weight 1: the same minutes land on the soft
        // level instead of the hard one.
        DecomposedCalculator calculator;
        auto component = std::make_unique<MaxEndDateTime>();
        component->isHard = false;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == 0
                  && score.soft == -breakdown.maxEndDateTime,
              "maxEndDateTime demoted to soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (e) FULL_ASSERT plugin solve ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 12);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(s, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(sameScore(s.score, recomputed),
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyConstraintTotals();
    verifyOverrides();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All food-packaging plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d food-packaging plugin check(s) FAILED.\n", failures);
    return 1;
}
