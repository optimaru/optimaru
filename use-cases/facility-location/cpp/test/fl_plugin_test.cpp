// Plugin-assembly (fl/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (including unassigned consumers)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: removing one component removes exactly that term
//   (d) weight / level overrides: capacity demoted to soft, setup weight
//       doubled, exponent shaping
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(problem, calculator) under FULL_ASSERT
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level guards
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "fl/constraints.hpp"
#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "fl/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace fl;

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
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

// Random mid-size instance, the fl_test shape: total capacity `slack` times
// the total demand, spread evenly over the facilities.
FacilityLocationProblem buildRandomInstance(int facilityCount,
                                            int consumerCount,
                                            std::uint64_t seed,
                                            std::int64_t demandEach = 15,
                                            std::int64_t slack = 5)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> latitudes(51.44, 51.56);
    std::uniform_real_distribution<double> longitudes(-0.16, -0.01);
    std::uniform_int_distribution<std::int64_t> setupCosts(20000, 80000);

    FacilityLocationProblem problem;
    problem.southWestCorner = {51.44, -0.16};
    problem.northEastCorner = {51.56, -0.01};
    const std::int64_t capacityEach
        = slack * demandEach * consumerCount / facilityCount;
    for (int f = 0; f < facilityCount; ++f)
    {
        Facility facility;
        facility.id = "F" + std::to_string(f);
        facility.location = {latitudes(rng), longitudes(rng)};
        facility.setupCost = setupCosts(rng);
        facility.capacity = capacityEach;
        problem.facilities.push_back(facility);
    }
    for (int c = 0; c < consumerCount; ++c)
    {
        Consumer consumer;
        consumer.id = "C" + std::to_string(c);
        consumer.location = {latitudes(rng), longitudes(rng)};
        consumer.demand = demandEach;
        problem.consumers.push_back(consumer);
    }
    return problem;
}

// Scores `problem`'s current assignment with a freshly reset assembly.
Score scoreWith(DecomposedCalculator &calculator,
                FacilityLocationProblem &problem)
{
    calculator.resetWorkingSolution(problem);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute, on random
// assignments (10% of the consumers left unassigned, like a partially
// initialized solution).
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 8,
                                                    /*consumers*/ 40,
                                                    /*seed*/ 7,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 1);
    ScoreCalculator::prepare(p);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (Consumer &consumer : p.consumers)
        {
            consumer.facility
                = unit(rng) < 0.10
                      ? -1
                      : static_cast<int>(rng() % p.facilityCount());
        }
        const Score plugin = scoreWith(calculator, p);
        const Score monolithic = ScoreCalculator::calculateScore(p);
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
          std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves, driven through the ScoreDirector so the plugin path's
// before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 8,
                                                    /*consumers*/ 40,
                                                    /*seed*/ 7,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 1);
    ScoreCalculator::prepare(p);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<FacilityLocationProblem> director(p, calculator);
    FacilityLocationProblem shadow = p;

    std::mt19937_64 rng(123);
    const int consumerCount = p.consumerCount();
    const int valueCount = p.facilityCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int consumer = static_cast<int>(rng() % consumerCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(consumer, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % consumerCount);
            const int b = static_cast<int>(rng() % consumerCount);
            const int va = p.consumers[a].facility;
            const int vb = p.consumers[b].facility;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < consumerCount; ++i)
            shadow.consumers[i].facility = p.consumers[i].facility;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
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
              + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Partial assemblies: dropping one component drops exactly that term.
// Hand-computed on a two-facility instance.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies ==\n");
    FacilityLocationProblem p;
    Facility near;
    near.id = "near";
    near.location = {0.0, 0.0};
    near.setupCost = 1000;
    near.capacity = 10;
    p.facilities.push_back(near);
    Facility far;
    far.id = "far";
    far.location = {1.0, 0.0};
    far.setupCost = 300;
    far.capacity = 100;
    p.facilities.push_back(far);
    for (int c = 0; c < 3; ++c)
    {
        Consumer consumer;
        consumer.id = "C" + std::to_string(c);
        consumer.location = {0.0, 0.0};
        consumer.demand = 6;
        consumer.facility = 0;  // all three on the small, near facility
        p.consumers.push_back(consumer);
    }
    ScoreCalculator::prepare(p);

    // 18 demand on a capacity of 10 -> 8 over; setup 1000; distance 0.
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -8 && std::fabs(score.soft - -2000.0) < 1e-9,
              "full assembly: -8hard / -2000soft (2 * 1000 setup)");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<FacilitySetupCost>();
        calculator.emplace<DistanceFromFacilityCost>();
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && std::fabs(score.soft - -2000.0) < 1e-9,
              "without FacilityCapacityCost: the hard term disappears");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<FacilityCapacityCost>();
        calculator.emplace<DistanceFromFacilityCost>();
        const Score score = scoreWith(calculator, p);
        check(score.hard == -8 && std::fabs(score.soft) < 1e-9,
              "without FacilitySetupCost: the setup term disappears");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and the weight are assembly-time
// metadata, not part of the cost.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    FacilityLocationProblem p;
    Facility only;
    only.id = "only";
    only.location = {0.0, 0.0};
    only.setupCost = 100;
    only.capacity = 5;
    p.facilities.push_back(only);
    Consumer consumer;
    consumer.id = "C0";
    consumer.location = {0.0, 0.0};
    consumer.demand = 9;
    consumer.facility = 0;
    p.consumers.push_back(consumer);
    ScoreCalculator::prepare(p);

    {
        // Capacity demoted to soft at weight 3: 4 over -> 12 soft, and the
        // hard level empties out.
        DecomposedCalculator calculator;
        auto capacity = std::make_unique<FacilityCapacityCost>(3.0);
        capacity->level = optimaru::Level::Soft;
        calculator.add(std::move(capacity));
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && std::fabs(score.soft - -12.0) < 1e-9,
              "FacilityCapacityCost demoted to soft at weight 3");
    }
    {
        // Doubling the setup weight doubles exactly that term.
        DecomposedCalculator calculator;
        calculator.emplace<FacilitySetupCost>(4.0);
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && std::fabs(score.soft - -400.0) < 1e-9,
              "FacilitySetupCost at weight 4");
    }
    {
        // exponent 2 shapes the raw cost before the weight: 4^2 = 16.
        DecomposedCalculator calculator;
        auto capacity = std::make_unique<FacilityCapacityCost>();
        capacity->exponent = 2.0;
        calculator.add(std::move(capacity));
        const Score score = scoreWith(calculator, p);
        check(score.hard == -16, "FacilityCapacityCost with exponent 2");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up to the score
// the same assembly reports.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 6,
                                                    /*consumers*/ 24,
                                                    /*seed*/ 13,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 1);
    ScoreCalculator::prepare(p);
    std::mt19937_64 rng(5);
    for (Consumer &consumer : p.consumers)
        consumer.facility = static_cast<int>(rng() % p.facilityCount());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, p);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        (total.isHard ? hard : soft) += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-9,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 3,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path: the engine cross-checks the
// incremental score against a full reset + recompute after every move.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 6,
                                                    /*consumers*/ 20,
                                                    /*seed*/ 11,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 2);

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
        solver.solve(p, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(p);
        check(sameScore(p.score, recomputed),
              "plugin-reported score == monolithic full recompute");
        check(p.score.hard == 0, "plugin solve reaches 0hard");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The assembly builder: stating the cost function by name produces the
// same calculator as listing the components by hand — and says it in one
// expression instead of five.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 6,
                                                    /*consumers*/ 24,
                                                    /*seed*/ 17,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 1);
    ScoreCalculator::prepare(p);
    std::mt19937_64 rng(3);
    for (Consumer &consumer : p.consumers)
        consumer.facility = static_cast<int>(rng() % p.facilityCount());

    // By hand: the way the same edit reads without the builder.
    DecomposedCalculator byHand;
    {
        auto capacity = std::make_unique<FacilityCapacityCost>();
        capacity->level = optimaru::Level::Soft;
        byHand.add(std::move(capacity));
        byHand.emplace<FacilitySetupCost>(4.0);
    }
    const Score expected = scoreWith(byHand, p);

    // By name: same three edits, one expression.
    DecomposedCalculator built;
    defaultConstraints()
        .soft("facilityCapacityCost")
        .weight("facilitySetupCost", 4)
        .drop("distanceFromFacilityCost")
        .into(built);
    check(sameScore(scoreWith(built, p), expected),
          "named edits == the same assembly listed by hand");

    // The default assembly, untouched, still matches the monolith.
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, p), ScoreCalculator::calculateScore(p)),
          "defaultConstraints() alone == the monolithic score");

    // A typo must not quietly leave the cost function as it was.
    bool threw = false;
    try
    {
        defaultConstraints().weight("facilitySetupCosts", 4);  // trailing s
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    check(threw,
          "a misspelled constraint name throws instead of doing nothing");

    // only() isolates a subset: just the hard rule, nothing else scored.
    DecomposedCalculator isolated;
    defaultConstraints().only({"facilityCapacityCost"}).into(isolated);
    const Score hardOnly = scoreWith(isolated, p);
    check(hardOnly.soft == 0.0 && hardOnly.hard != 0,
          "only() scores exactly the named constraint");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraintCosts() — the raw per-unit numbers before weight and level.
// Degree offsets are binary-exact (0.25 deg = 27750 m, see fl_test).
// -------------------------------------------------------------------------
std::vector<double> rawCosts(FacilityLocationProblem &problem,
                             const std::string &name)
{
    ScoreCalculator::prepare(problem);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(problem);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

// F0 at the origin (setup 100, capacity 10), F1 0.25 deg east (setup 40,
// capacity 50); each case places its own consumers.
FacilityLocationProblem twoFacilities()
{
    FacilityLocationProblem p;
    for (int f = 0; f < 2; ++f)
    {
        Facility facility;
        facility.id = "F" + std::to_string(f);
        facility.location = {0.0, f == 0 ? 0.0 : 0.25};
        facility.setupCost = f == 0 ? 100 : 40;
        facility.capacity = f == 0 ? 10 : 50;
        p.facilities.push_back(facility);
    }
    return p;
}

void addConsumerAt(FacilityLocationProblem &p,
                   double longitude,
                   std::int64_t demand,
                   int facility)
{
    Consumer consumer;
    consumer.id = "C" + std::to_string(p.consumers.size());
    consumer.location = {0.0, longitude};
    consumer.demand = demand;
    consumer.facility = facility;
    p.consumers.push_back(consumer);
}

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        // F0 carries 8 + 7 = 15 on capacity 10 -> 5; F1 carries 3 on 50 -> 0.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 8, 0);
        addConsumerAt(p, 0.0, 7, 0);
        addConsumerAt(p, 0.25, 3, 1);
        check(rawCosts(p, "facilityCapacityCost")
                  == std::vector<double>{5.0, 0.0},
              "facilityCapacityCost: [5, 0] (15 on 10; 3 on 50)");
    }
    {
        // Exactly at capacity on both facilities.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 10, 0);
        addConsumerAt(p, 0.25, 50, 1);
        check(rawCosts(p, "facilityCapacityCost")
                  == std::vector<double>{0.0, 0.0},
              "facilityCapacityCost: [0, 0] exactly at capacity");
    }
    {
        // Only F1 is used, by two consumers: its setup is paid once.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.25, 1, 1);
        addConsumerAt(p, 0.25, 1, 1);
        check(rawCosts(p, "facilitySetupCost")
                  == std::vector<double>{0.0, 40.0},
              "facilitySetupCost: [0, 40] (F1 used twice, paid once)");
    }
    {
        // Nothing assigned, nothing open.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 1, -1);
        check(rawCosts(p, "facilitySetupCost") == std::vector<double>{0.0, 0.0},
              "facilitySetupCost: [0, 0] with every consumer unassigned");
    }
    {
        // A consumer at the origin served by F1 (0.25 deg away) -> 27750 m
        // on F1; F0's own consumer stands on it.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 1, 0);
        addConsumerAt(p, 0.0, 1, 1);
        check(rawCosts(p, "distanceFromFacilityCost")
                  == std::vector<double>{0.0, 27750.0},
              "distanceFromFacilityCost: [0, 27750]");
    }
    {
        // Every consumer on top of its facility.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 1, 0);
        addConsumerAt(p, 0.25, 1, 1);
        check(rawCosts(p, "distanceFromFacilityCost")
                  == std::vector<double>{0.0, 0.0},
              "distanceFromFacilityCost: [0, 0] on top of the facilities");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog read off the default assembly is exactly the components'
// constants, and the level/weight guards hold on the domain's assembly.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable && !row.reward && !row.isGlobal;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    const auto rows = defaultConstraints().info();
    check(rows.size() == 3
              && rowIs<FacilityCapacityCost>(
                  rows[0], "facilityCapacityCost", optimaru::Level::Hard, 1.0)
              && rowIs<FacilitySetupCost>(
                  rows[1], "facilitySetupCost", optimaru::Level::Soft, 2.0)
              && rowIs<DistanceFromFacilityCost>(rows[2],
                                                 "distanceFromFacilityCost",
                                                 optimaru::Level::Soft,
                                                 6.0),
          "info(): capacity hard 1, setup soft 2, distance soft 6; all "
          "flippable, no reward, no global");
    check(!throwsInvalid([] { defaultConstraints().soft("facilityCapacityCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("distanceFromFacilityCost"); }),
          "every level flips both ways");
    check(throwsInvalid(
              [] { defaultConstraints().weight("facilitySetupCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid(
              [] { defaultConstraints().exponent("facilitySetupCost", 0.0); }),
          "a non-positive exponent is refused");
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
    verifyAssemblyBuilder();
    verifyRawCostsByHand();
    verifyCatalog();

    if (failures == 0)
    {
        std::printf("All facility-location plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d facility-location plugin check(s) FAILED.\n", failures);
    return 1;
}
