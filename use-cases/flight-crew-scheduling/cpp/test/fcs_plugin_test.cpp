// Plugin-assembly (fcs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned seats included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level overrides: re-weighted, demoted to soft, promoted to
//       hard, exponent shaping
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level/weight guards
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (conflicts + their square), threshold / literal / negative
//       offsets by hand (an idle employee counts in literal mode), per-unit
//       hard rounding, the literal formula refused on hard at reset and kept
//       on soft, incremental == a fresh recompute over a random walk (delta,
//       full-recost and soft components repeated and shifted) and a
//       FULL_ASSERT solve with the same assembly
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "fcs/constraints.hpp"
#include "fcs/domain.hpp"
#include "fcs/score_calculator.hpp"
#include "fcs/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace fcs;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                long long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-48s got=%s expected=%lldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

FlightCrewSchedule makeBase(int airportCount, int employeeCount)
{
    FlightCrewSchedule s;
    for (int a = 0; a < airportCount; ++a)
        s.airports.push_back({"A" + std::to_string(a), "", 0.0, 0.0});
    for (int e = 0; e < employeeCount; ++e)
    {
        Employee employee;
        employee.id = std::to_string(e);
        employee.name = "E" + std::to_string(e);
        employee.homeAirport = 0;
        employee.skills = {"Pilot"};
        s.employees.push_back(employee);
    }
    return s;
}

int addFlight(FlightCrewSchedule &s,
              int departureAirport,
              Minutes departure,
              int arrivalAirport,
              Minutes arrival)
{
    Flight flight;
    flight.flightNumber = "F" + std::to_string(s.flights.size());
    flight.departureAirport = departureAirport;
    flight.departureUTC = departure;
    flight.arrivalAirport = arrivalAirport;
    flight.arrivalUTC = arrival;
    s.flights.push_back(flight);
    return static_cast<int>(s.flights.size()) - 1;
}

int addAssignment(FlightCrewSchedule &s,
                  int flight,
                  int employee,
                  const std::string &requiredSkill = "Pilot")
{
    FlightAssignment assignment;
    assignment.id = std::to_string(s.flightAssignments.size());
    assignment.flight = flight;
    assignment.indexInFlight = static_cast<int>(s.flightAssignments.size()) + 1;
    assignment.requiredSkill = requiredSkill;
    assignment.employee = employee;
    s.flightAssignments.push_back(assignment);
    return static_cast<int>(s.flightAssignments.size()) - 1;
}

FlightCrewSchedule
buildRandomInstance(int flightCount, int employeeCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    FlightCrewSchedule s = makeBase(4, employeeCount);
    const std::vector<std::string> skills = {"Pilot", "Flight attendant"};
    for (int e = 0; e < employeeCount; ++e)
    {
        s.employees[e].homeAirport = static_cast<int>(rng() % 4);
        s.employees[e].skills = {skills[rng() % 2]};
        if (rng() % 4 == 0)
            s.employees[e].unavailableDays = {static_cast<DayIndex>(rng() % 3)};
    }
    for (int f = 0; f < flightCount; ++f)
    {
        const int dep = static_cast<int>(rng() % 4);
        const int arr = static_cast<int>(rng() % 4);  // same airport allowed
        const Minutes depTime = static_cast<Minutes>(rng() % (3 * 24)) * 60;
        const Minutes duration = 60 + static_cast<Minutes>(rng() % (12 * 60));
        const int flight = addFlight(s, dep, depTime, arr, depTime + duration);
        addAssignment(s, flight, -1, skills[rng() % 2]);
        if (rng() % 2 == 0)
            addAssignment(s, flight, -1, skills[rng() % 2]);
    }
    return s;
}

Score scoreWith(DecomposedCalculator &calculator, FlightCrewSchedule &s)
{
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.resetWorkingSolution(s);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 18, /*employees*/ 8, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int employeeCount = static_cast<int>(s.employees.size());
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (FlightAssignment &assignment : s.flightAssignments)
        {
            assignment.employee = unit(rng) < 0.10
                                      ? -1
                                      : static_cast<int>(rng() % employeeCount);
        }
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
          std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 18, /*employees*/ 8, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<FlightCrewSchedule> director(s, calculator);
    FlightCrewSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int assignmentCount = static_cast<int>(s.flightAssignments.size());
    const int valueCount = static_cast<int>(s.employees.size());
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(assignment, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.flightAssignments[a].employee;
            const int vb = s.flightAssignments[b].employee;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < assignmentCount; ++i)
            shadow.flightAssignments[i].employee
                = s.flightAssignments[i].employee;
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
// (c) Single-component assemblies on hand-computed instances.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // requiredSkillCost alone: the employee is a Pilot, the seat wants "1".
        FlightCrewSchedule s = makeBase(2, 1);
        const int f = addFlight(s, 0, 0, 1, 60);
        addAssignment(s, f, 0, "1");
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkillCost>();
        checkScore(
            "requiredSkillCost alone", scoreWith(calculator, s), -100, 0.0);
    }
    {  // The right skill: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        const int f = addFlight(s, 0, 0, 1, 60);
        addAssignment(s, f, 0, "Pilot");
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkillCost>();
        checkScore(
            "requiredSkillCost satisfied", scoreWith(calculator, s), 0, 0.0);
    }
    {  // flightConflictCost alone: two overlapping flights on one employee.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 1, 0, 11), 0);
        DecomposedCalculator calculator;
        calculator.emplace<FlightConflictCost>();
        checkScore(
            "flightConflictCost alone", scoreWith(calculator, s), -10, 0.0);
    }
    {  // Back to back, not overlapping: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 20), 0);
        DecomposedCalculator calculator;
        calculator.emplace<FlightConflictCost>();
        checkScore("flightConflictCost, back to back",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // transferBetweenTwoFlightsCost alone: lands at airport 1, next leaves
        // from airport 0.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 0, 20, 0, 30), 0);
        DecomposedCalculator calculator;
        calculator.emplace<TransferBetweenTwoFlightsCost>();
        checkScore("transferBetweenTwoFlightsCost alone",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {  // Continuing from the arrival airport: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        DecomposedCalculator calculator;
        calculator.emplace<TransferBetweenTwoFlightsCost>();
        checkScore("transferBetweenTwoFlightsCost, connected",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // employeeUnavailabilityCost alone: a day-0 flight on an employee who is
        // unavailable that day.
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {0};
        addAssignment(s, addFlight(s, 0, 0, 1, 60), 0);
        DecomposedCalculator calculator;
        calculator.emplace<EmployeeUnavailabilityCost>();
        checkScore("employeeUnavailabilityCost alone",
                   scoreWith(calculator, s),
                   -10,
                   0.0);
    }
    {  // The two home-airport rules: the single flight is both first and
        // last, leaving from airport 1 (home is 0) and landing at 1.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 1, 0, 1, 60), 0);
        DecomposedCalculator first;
        first.emplace<FirstAssignmentNotDepartingFromHomeCost>();
        checkScore("firstAssignmentNotDepartingFromHomeCost alone",
                   scoreWith(first, s),
                   0,
                   -1000.0);
        DecomposedCalculator last;
        last.emplace<LastAssignmentNotArrivingAtHomeCost>();
        checkScore("lastAssignmentNotArrivingAtHomeCost alone",
                   scoreWith(last, s),
                   0,
                   -1000.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    FlightCrewSchedule s = makeBase(2, 1);
    addAssignment(s, addFlight(s, 0, 0, 1, 60), 0, "1");

    {
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkillCost>(1.0);
        checkScore("requiredSkillCost re-weighted to 1",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredSkillCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("requiredSkillCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -100.0);
    }
    {
        // Two violating seats, exponent 2: the raw count (2) is shaped before
        // the weight -> 4 * 100.
        FlightCrewSchedule two = makeBase(2, 1);
        addAssignment(two, addFlight(two, 0, 0, 1, 60), 0, "1");
        addAssignment(two, addFlight(two, 0, 100, 1, 160), 0, "1");
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredSkillCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("requiredSkillCost with exponent 2",
                   scoreWith(calculator, two),
                   -400,
                   0.0);
    }
    {
        // A soft rule promoted to hard keeps its weight in the new level:
        // the single flight leaves from airport 1 (home 0) -> 1000 hard.
        FlightCrewSchedule away = makeBase(2, 1);
        addAssignment(away, addFlight(away, 1, 0, 0, 60), 0);
        DecomposedCalculator calculator;
        auto component
            = std::make_unique<FirstAssignmentNotDepartingFromHomeCost>();
        component->level = optimaru::Level::Hard;
        calculator.add(std::move(component));
        checkScore("firstAssignmentNotDepartingFromHomeCost promoted to hard",
                   scoreWith(calculator, away),
                   -1000,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 12, /*employees*/ 6, /*seed*/ 13);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (FlightAssignment &assignment : s.flightAssignments)
        assignment.employee = static_cast<int>(rng() % s.employees.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, s);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 6,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 12, /*employees*/ 6, /*seed*/ 11);

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


// -------------------------------------------------------------------------
// (g) The defaultConstraints() builder: named edits == by-hand assembly.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 12, /*employees*/ 6, /*seed*/ 17);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(21);
    for (FlightAssignment &assignment : s.flightAssignments)
        assignment.employee = static_cast<int>(rng() % s.employees.size());

    // By hand: the default set with transfer demoted to soft, unavailability
    // re-weighted to 2 and the last-not-home rule left out.
    DecomposedCalculator byHand;
    {
        byHand.emplace<RequiredSkillCost>();
        byHand.emplace<FlightConflictCost>();
        auto transfer = std::make_unique<TransferBetweenTwoFlightsCost>();
        transfer->level = optimaru::Level::Soft;
        byHand.add(std::move(transfer));
        byHand.emplace<EmployeeUnavailabilityCost>(2.0);
        byHand.emplace<FirstAssignmentNotDepartingFromHomeCost>();
    }
    const Score expected = scoreWith(byHand, s);

    // By name: the same three edits, one expression.
    DecomposedCalculator built;
    defaultConstraints()
        .soft("transferBetweenTwoFlightsCost")
        .weight("employeeUnavailabilityCost", 2)
        .drop("lastAssignmentNotArrivingAtHomeCost")
        .into(built);
    check(sameScore(scoreWith(built, s), expected),
          "named edits == the same assembly listed by hand");

    // The default assembly, untouched, still matches the monolith.
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, s), ScoreCalculator::calculateScore(s)),
          "defaultConstraints() alone == the monolithic score");

    // A typo must not quietly leave the cost function as it was.
    bool threw = false;
    try
    {
        defaultConstraints().weight("requiredSkillCosts", 4);  // trailing s
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    check(threw, "an unknown name is refused (std::invalid_argument)");

    // only() isolates a subset: just the two home-airport rules.
    DecomposedCalculator isolated;
    defaultConstraints()
        .only({"firstAssignmentNotDepartingFromHomeCost",
               "lastAssignmentNotArrivingAtHomeCost"})
        .into(isolated);
    DecomposedCalculator homeRules;
    homeRules.emplace<FirstAssignmentNotDepartingFromHomeCost>();
    homeRules.emplace<LastAssignmentNotArrivingAtHomeCost>();
    const Score softOnly = scoreWith(isolated, s);
    check(softOnly.hard == 0
              && sameScore(softOnly, scoreWith(homeRules, s)),
          "only() scores exactly the named constraints");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraintCosts() — the raw per-employee numbers before weight and level.
// -------------------------------------------------------------------------
std::vector<double> rawCosts(FlightCrewSchedule &schedule,
                             const std::string &name)
{
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(schedule);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    using Costs = std::vector<double>;
    {
        // E0 holds two seats wanting "Attendant" (it is a Pilot) and one
        // Pilot seat -> 2; E1 holds one Pilot seat -> 0.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 0, 60), 0, "Attendant");
        addAssignment(s, addFlight(s, 0, 100, 0, 160), 0, "Attendant");
        addAssignment(s, addFlight(s, 0, 200, 0, 260), 0, "Pilot");
        addAssignment(s, addFlight(s, 0, 300, 0, 360), 1, "Pilot");
        check(rawCosts(s, "requiredSkillCost") == Costs{2.0, 0.0},
              "requiredSkillCost: [2, 0] (two unqualified seats on E0)");
    }
    {
        // Every seat matches the crew member's skill.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 0, 60), 0, "Pilot");
        addAssignment(s, addFlight(s, 0, 100, 0, 160), 1, "Pilot");
        check(rawCosts(s, "requiredSkillCost") == Costs{0.0, 0.0},
              "requiredSkillCost: [0, 0] all qualified");
    }
    {
        // E0: [0,100) x [10,100) x [20,100) -> C(3,2) = 3 pairs; E1 alone.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 0, 100), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 100), 0);
        addAssignment(s, addFlight(s, 0, 20, 0, 100), 0);
        addAssignment(s, addFlight(s, 0, 0, 0, 100), 1);
        check(rawCosts(s, "flightConflictCost") == Costs{3.0, 0.0},
              "flightConflictCost: [3, 0] (three mutually overlapping)");
    }
    {
        // Touching flights ([0,10) then [10,20)) do not overlap.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 20), 0);
        check(rawCosts(s, "flightConflictCost") == Costs{0.0, 0.0},
              "flightConflictCost: [0, 0] back to back");
    }
    {
        // E0 lands at 1 twice and leaves from 0 next: 0->1, 0->1, 0->1 ->
        // two broken connections (the Java case); E1 has no flights.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 0, 11, 1, 12), 0);
        addAssignment(s, addFlight(s, 0, 13, 1, 14), 0);
        check(rawCosts(s, "transferBetweenTwoFlightsCost") == Costs{2.0, 0.0},
              "transferBetweenTwoFlightsCost: [2, 0] (Java case)");
    }
    {
        // A connected chain 0->1, 1->0.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        check(rawCosts(s, "transferBetweenTwoFlightsCost") == Costs{0.0, 0.0},
              "transferBetweenTwoFlightsCost: [0, 0] connected chain");
    }
    {
        // E1 is unavailable on day 5: one flight departs that day, one
        // departs day 4 and lands on day 5, one flies on day 6 -> 2.
        FlightCrewSchedule s = makeBase(2, 2);
        s.employees[1].unavailableDays = {5};
        const Minutes day = MINUTES_PER_DAY;
        addAssignment(s, addFlight(s, 0, 5 * day, 0, 5 * day + 10), 1);
        addAssignment(s, addFlight(s, 0, 4 * day, 0, 5 * day + 10), 1);
        addAssignment(s, addFlight(s, 0, 6 * day, 0, 6 * day + 10), 1);
        addAssignment(s, addFlight(s, 0, 5 * day, 0, 5 * day + 10), 0);
        check(rawCosts(s, "employeeUnavailabilityCost") == Costs{0.0, 2.0},
              "employeeUnavailabilityCost: [0, 2] (day 5 at departure and "
              "during a flight)");
    }
    {
        // Flying on the day after the unavailable one.
        FlightCrewSchedule s = makeBase(2, 2);
        s.employees[0].unavailableDays = {5};
        const Minutes day = MINUTES_PER_DAY;
        addAssignment(s, addFlight(s, 0, 6 * day, 0, 6 * day + 10), 0);
        check(rawCosts(s, "employeeUnavailabilityCost") == Costs{0.0, 0.0},
              "employeeUnavailabilityCost: [0, 0] available");
    }
    {
        // E0 (home 0): two flights tied on the earliest departure, both
        // leaving airport 1 -> 2 (ties all count); E1 (home 0) leaves home.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 1, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 1, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 1);
        check(rawCosts(s, "firstAssignmentNotDepartingFromHomeCost")
                  == Costs{2.0, 0.0},
              "firstAssignmentNotDepartingFromHomeCost: [2, 0] (a tie)");
    }
    {
        // Both rosters start at home (E1 has none at all).
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 1, 30), 0);
        check(rawCosts(s, "firstAssignmentNotDepartingFromHomeCost")
                  == Costs{0.0, 0.0},
              "firstAssignmentNotDepartingFromHomeCost: [0, 0] from home");
    }
    {
        // E1 (home 0) ends the roster at airport 1; E0 ends at home.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 1);
        check(rawCosts(s, "lastAssignmentNotArrivingAtHomeCost")
                  == Costs{0.0, 1.0},
              "lastAssignmentNotArrivingAtHomeCost: [0, 1]");
    }
    {
        // A home round trip.
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        check(rawCosts(s, "lastAssignmentNotArrivingAtHomeCost")
                  == Costs{0.0, 0.0},
              "lastAssignmentNotArrivingAtHomeCost: [0, 0] back home");
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
           && row.levelFlippable && !row.reward && !row.isGlobal
           && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 6
              && rowIs<RequiredSkillCost>(
                  rows[0], "requiredSkillCost", Level::Hard, 100.0)
              && rowIs<FlightConflictCost>(
                  rows[1], "flightConflictCost", Level::Hard, 10.0)
              && rowIs<TransferBetweenTwoFlightsCost>(
                  rows[2], "transferBetweenTwoFlightsCost", Level::Hard, 1.0)
              && rowIs<EmployeeUnavailabilityCost>(
                  rows[3], "employeeUnavailabilityCost", Level::Hard, 10.0)
              && rowIs<FirstAssignmentNotDepartingFromHomeCost>(
                  rows[4],
                  "firstAssignmentNotDepartingFromHomeCost",
                  Level::Soft,
                  1000.0)
              && rowIs<LastAssignmentNotArrivingAtHomeCost>(
                  rows[5],
                  "lastAssignmentNotArrivingAtHomeCost",
                  Level::Soft,
                  1000.0),
          "info(): hard 100 / 10 / 1 / 10, soft 1000 / 1000; all "
          "flippable, no reward, no global, all default-enabled");
    check(!throwsInvalid([] { defaultConstraints().soft("requiredSkillCost"); })
              && !throwsInvalid(
                  []
                  {
                      defaultConstraints().hard(
                          "lastAssignmentNotArrivingAtHomeCost");
                  }),
          "every level flips both ways");
    check(throwsInvalid(
              [] { defaultConstraints().weight("flightConflictCost", -1.0); }),
          "a negative weight is refused");
    check(throwsInvalid(
              [] { defaultConstraints().exponent("flightConflictCost", 0.0); }),
          "a non-positive exponent is refused");
    std::printf("\n");
}


// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Three employees at home airport 0: E0 flies three mutually overlapping
// round trips (3 conflict pairs), E1 one flight from airport 1 home that
// wants an "Attendant" (1 unqualified seat, 1 first flight not from home),
// E2 stays idle. Raw per employee: conflicts [3, 0, 0], skill [0, 1, 0],
// first not home [0, 1, 0].
FlightCrewSchedule repeatInstance()
{
    FlightCrewSchedule s = makeBase(2, 3);
    addAssignment(s, addFlight(s, 0, 0, 0, 100), 0);
    addAssignment(s, addFlight(s, 0, 10, 0, 100), 0);
    addAssignment(s, addFlight(s, 0, 20, 0, 100), 0);
    addAssignment(s, addFlight(s, 1, 200, 0, 260), 1, "Attendant");
    return s;
}

Score repeatScore(ConstraintAssembly assembly)
{
    FlightCrewSchedule s = repeatInstance();
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    return scoreWith(calculator, s);
}

// The four hard components take threshold offsets on hard; their literal
// copies sit on soft, because hard refuses the literal formula with an
// offset (hard 0 must mean no violation).
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("flightConflictCost", 1.0);
    assembly.offset("requiredSkillCost", -1.0);
    assembly.addInstances(std::move(copyOf("flightConflictCost")
                                        .soft("flightConflictCost")
                                        .offset("flightConflictCost", 1.0)
                                        .clip("flightConflictCost", false)));
    assembly.addInstances(std::move(copyOf("flightConflictCost")
                                        .weight("flightConflictCost", 2.0)
                                        .exponent("flightConflictCost", 2.0)
                                        .offset("flightConflictCost", 1.0)));
    assembly.addInstances(std::move(copyOf("requiredSkillCost")
                                        .soft("requiredSkillCost")
                                        .weight("requiredSkillCost", 30.0)
                                        .offset("requiredSkillCost", 0.5)
                                        .clip("requiredSkillCost", false)));
    assembly.addInstances(std::move(copyOf("requiredSkillCost")
                                        .weight("requiredSkillCost", 30.0)
                                        .offset("requiredSkillCost", 0.5)));
    assembly.addInstances(std::move(copyOf("employeeUnavailabilityCost")
                                        .soft("employeeUnavailabilityCost")
                                        .offset("employeeUnavailabilityCost", -2.0)
                                        .clip("employeeUnavailabilityCost", false)));
    assembly.addInstances(std::move(
        copyOf("employeeUnavailabilityCost").offset("employeeUnavailabilityCost", -2.0)));
    assembly.addInstances(std::move(copyOf("transferBetweenTwoFlightsCost")
                                        .weight("transferBetweenTwoFlightsCost", 3.0)
                                        .offset("transferBetweenTwoFlightsCost", 1.0)));
    assembly.addInstances(std::move(copyOf("firstAssignmentNotDepartingFromHomeCost")
                                        .weight("firstAssignmentNotDepartingFromHomeCost", 0.25)
                                        .offset("firstAssignmentNotDepartingFromHomeCost", 0.5)
                                        .clip("firstAssignmentNotDepartingFromHomeCost", false)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        FlightCrewSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("flightConflictCost");
        assembly.addInstances(std::move(copyOf("flightConflictCost")
                                            .weight("flightConflictCost", 2.0)
                                            .exponent("flightConflictCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("10 x conflicts + 2 x conflicts^2 per employee: -(30 + 18)",
                   scoreWith(calculator, s), -48, 0.0);
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{3.0, 0.0, 0.0}
                  && entries[1].weight == 2.0 && entries[1].exponent == 2.0
                  && entries[1].defaultWeight == 10.0 && !entries[0].scoped,
              "two instances, the same raw costs, each with its own knobs, every employee "
              "evaluated");
    }
    checkScore("threshold offset 2 on the conflicts: 10 x 1",
               repeatScore(copyOf("flightConflictCost").offset("flightConflictCost", 2.0)), -10,
               0.0);
    // The literal formula with an offset is refused on hard (below), so the
    // literal picks of the hard components are scored on soft.
    checkScore("literal offset 2 on the conflicts, on soft: 10 x (1 - 2 - 2), the idle E2 "
               "included",
               repeatScore(copyOf("flightConflictCost")
                               .soft("flightConflictCost")
                               .offset("flightConflictCost", 2.0)
                               .clip("flightConflictCost", false)),
               0, 30.0);
    checkScore("threshold offset -1 on the skill: 100 x (1 + 1), the others 0",
               repeatScore(copyOf("requiredSkillCost").offset("requiredSkillCost", -1.0)), -200,
               0.0);
    checkScore("literal offset -1 on the skill, on soft: 100 x (1 + 2 + 1)",
               repeatScore(copyOf("requiredSkillCost")
                               .soft("requiredSkillCost")
                               .offset("requiredSkillCost", -1.0)
                               .clip("requiredSkillCost", false)),
               0, -400.0);
    {
        // A hard pick rounds per employee: with E1 on two more overlapping
        // flights the conflicts are [3, 1, 0], and at weight 0.5 llround(1.5)
        // + llround(0.5) = 3 (a rounded total would give 2).
        FlightCrewSchedule s = repeatInstance();
        addAssignment(s, addFlight(s, 0, 300, 0, 400), 1);
        addAssignment(s, addFlight(s, 0, 310, 0, 410), 1);
        DecomposedCalculator calculator;
        copyOf("flightConflictCost").weight("flightConflictCost", 0.5).into(calculator);
        checkScore("conflicts [3, 1, 0] at weight 0.5: llround per employee, -3 hard",
                   scoreWith(calculator, s), -3, 0.0);
    }
    checkScore("a soft literal offset 1: 1000 x (-1 + 0 - 1) -> +2000 soft",
               repeatScore(copyOf("firstAssignmentNotDepartingFromHomeCost")
                               .offset("firstAssignmentNotDepartingFromHomeCost", 1.0)
                               .clip("firstAssignmentNotDepartingFromHomeCost", false)),
               0, 2000.0);
    checkScore("identical instances add up: 2 x 100",
               repeatScore(std::move(
                   copyOf("requiredSkillCost").addInstances(copyOf("requiredSkillCost")))),
               -200, 0.0);
    {
        // On hard (the default of the four hard components) the literal
        // formula with an offset is refused at reset, whichever sign and
        // setter order: +2 would reward E1 and E2 (+30 hard, so their slack
        // would cancel E0's conflicts), -1 would charge employees that hold
        // no violation.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            try
            {
                repeatScore(std::move(assembly));
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level that counts toward "
                     "the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("flightConflictCost")
                               .offset("flightConflictCost", 2.0)
                               .clip("flightConflictCost", false))
                      == refusal("flightConflictCost")
                  && resetRefusal(copyOf("requiredSkillCost")
                                      .clip("requiredSkillCost", false)
                                      .offset("requiredSkillCost", -1.0))
                         == refusal("requiredSkillCost"),
              "conflicts / skill literal offset on hard (their default): refused at reset");
        check(resetRefusal(copyOf("firstAssignmentNotDepartingFromHomeCost")
                               .offset("firstAssignmentNotDepartingFromHomeCost", 1.0)
                               .clip("firstAssignmentNotDepartingFromHomeCost", false)
                               .hard("firstAssignmentNotDepartingFromHomeCost"))
                  == refusal("firstAssignmentNotDepartingFromHomeCost"),
              "a soft component's literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("flightConflictCost");
        repeated.addInstances(std::move(copyOf("flightConflictCost")
                                            .offset("flightConflictCost", 1.0)
                                            .clip("flightConflictCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("flightConflictCost"),
              "a repeated conflict instance with the literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("flightConflictCost").offset("flightConflictCost", 2.0)).empty()
                  && resetRefusal(copyOf("flightConflictCost").clip("flightConflictCost", false))
                         .empty(),
              "allowed on hard: a threshold offset, or clip=false with offset 0");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        FlightCrewSchedule s = buildRandomInstance(/*flights*/ 18, /*employees*/ 8, /*seed*/ 13);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::ScoreDirector<FlightCrewSchedule> director(s, calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int assignmentCount = static_cast<int>(s.flightAssignments.size());
        const int valueCount = static_cast<int>(s.employees.size());
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value
                = unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % valueCount);
            director.changeVariable(assignment, value);
            FlightCrewSchedule shadow = s;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, shadow);
            const Score incremental = director.calculateScore();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scoresAgree(incremental, full))
                ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random moves with repeated, shifted components: incremental == fresh");
    }
    {
        FlightCrewSchedule s = buildRandomInstance(/*flights*/ 12, /*employees*/ 6, /*seed*/ 11);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 3;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
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
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && optimaru::scoresAgree(s.score, scoreWith(fresh, s)),
              "the reported score == a fresh recompute of the same assembly");
    }
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
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All flight-crew-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d flight-crew-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}
