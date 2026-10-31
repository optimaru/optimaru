// Plugin-assembly (pdp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random route
//       layouts (unassigned visits included)
//   (b) incremental score == full recompute across 1500 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component isolates its own term
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under FULL_ASSERT
//   (g) the pair move takes a misplaced bound request to its vehicle
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "pdp/constraints.hpp"
#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace pdp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

Score scoreWith(DecomposedCalculator &calculator, PdpRoutePlan &plan)
{
    calculator.resetWorkingSolution(plan);
    return fromEngineScore(calculator.calculateScore());
}

// Spreads every visit over the vehicles at random; `poolRate` of them go to
// the unassigned pool list and another `poolRate` into no list at all.
void randomizeRoutes(PdpRoutePlan &plan, std::mt19937_64 &rng, double poolRate)
{
    for (Vehicle &vehicle : plan.vehicles)
        vehicle.nodeIndices.clear();
    plan.unassignedNodeIndices.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int v = 0; v < static_cast<int>(plan.nodes.size()); ++v)
    {
        const double roll = unit(rng);
        if (roll < poolRate)
        {
            plan.unassignedNodeIndices.push_back(v);
            continue;
        }
        if (roll < 2 * poolRate)
            continue;  // in no list at all
        plan.vehicles[rng() % plan.vehicles.size()].nodeIndices.push_back(v);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round)
    {
        randomizeRoutes(plan, rng, /*poolRate=*/0.10);
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::calculateScore(plan);
        if (!(plugin == monolithic))
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
          std::to_string(rounds) + " random route layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);
    plan.rebuildUnassignedPool();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<PdpRoutePlan> director(plan, calculator);

    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;
    std::mt19937_64 rng(3);
    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step)
    {
        const int listCount = Descriptor::listCount(plan);
        const int from
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        if (Descriptor::listSize(plan, from) == 0)
            continue;
        const int position = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
        const int element = Descriptor::getElement(plan, from, position);
        director.removeListElement(from, position);

        const int to
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        const int insertAt = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
        director.insertListElement(to, insertAt, element);

        const Score incremental = fromEngineScore(director.calculateScore());
        const Score full = ScoreCalculator::calculateScore(plan);
        if (!(incremental == full))
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
// (c) Each component in isolation carries exactly its own level.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");
    PdpRoutePlan plan = buildRandomInstance(16, 3, /*seed=*/21);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.15);
    const Score full = ScoreCalculator::calculateScore(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<PairedRequest>();
        calculator.emplace<SameVehicle>();
        calculator.emplace<PickupBeforeDropoff>();
        calculator.emplace<VehicleCapacity>();
        calculator.emplace<ServiceFinishedAfterMaxEndTime>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == full.hard && score.medium == 0 && score.soft == 0,
              "the five hard components alone reproduce the hard level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedRequests>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == full.medium && score.soft == 0,
              "unassignedRequests alone reproduces the medium level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeTravelTime>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == full.soft,
              "minimizeTravelTime alone reproduces the soft level");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    PdpRoutePlan plan = buildRandomInstance(16, 3, /*seed=*/31);
    std::mt19937_64 rng(7);
    randomizeRoutes(plan, rng, /*poolRate=*/0.05);

    DecomposedCalculator baseline;
    baseline.emplace<VehicleCapacity>();
    const Score base = scoreWith(baseline, plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacity>(10.0);
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 10 * base.hard,
              "vehicleCapacity re-weighted to 10");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<VehicleCapacity>();
        component->isHard = false;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == base.hard,
              "vehicleCapacity demoted to soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);

    const std::size_t componentCount = defaultConstraints().size();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == componentCount,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    plan.rebuildUnassignedPool();

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
        solver.solve(plan, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(plan);
        check(plan.score == recomputed,
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The pair move takes a misplaced bound request straight to its vehicle.
// -------------------------------------------------------------------------
void verifyRepairHonoursRequiredVehicle()
{
    std::printf("== (g) Repair and requiredVehicle ==\n");
    // A single request, so no stop is routed near it and the pair move falls
    // back on picking a vehicle. With only that move and a handful of steps, a
    // pick among all the vehicles would almost never find the right one.
    const int vehicleCount = 40;
    int repaired = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed)
    {
        PdpRoutePlan plan = buildRandomInstance(1, vehicleCount, /*seed=*/21);
        plan.deliveries[0].requiredVehicleIndex = vehicleCount - 1;
        plan.vehicles[0].nodeIndices = {plan.deliveries[0].pickupNodeIndex,
                                        plan.deliveries[0].dropoffNodeIndex};

        Solver::Config config;
        config.stepCountLimit = 8;
        config.spentLimit = std::chrono::minutes(1);  // safety net only
        config.randomSeed = static_cast<std::uint64_t>(seed);
        config.pairAssignmentProbability = 0.0;
        config.relocateMoveWeight = 0.0;
        config.listSwapMoveWeight = 0.0;
        config.twoOptMoveWeight = 0.0;
        config.orOptMoveWeight = 0.0;
        config.tailSwapMoveWeight = 0.0;
        config.ruinRecreate = false;

        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(plan, calculator);
        if (plan.vehicles[vehicleCount - 1].nodeIndices.size() == 2)
            ++repaired;
    }
    check(repaired == seeds, "a misplaced bound request reaches its vehicle");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();
    verifyRepairHonoursRequiredVehicle();

    if (failures == 0)
    {
        std::printf("All pickup-delivery plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d pickup-delivery plugin check(s) FAILED.\n", failures);
    return 1;
}
