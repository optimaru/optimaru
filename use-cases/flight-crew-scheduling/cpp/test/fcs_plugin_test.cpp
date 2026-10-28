// Plugin-assembly (fcs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned seats included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
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

    {  // requiredSkill alone: the employee is a Pilot, the seat wants "1".
        FlightCrewSchedule s = makeBase(2, 1);
        const int f = addFlight(s, 0, 0, 1, 60);
        addAssignment(s, f, 0, "1");
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkill>();
        checkScore("requiredSkill alone", scoreWith(calculator, s), -100, 0.0);
    }
    {  // The right skill: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        const int f = addFlight(s, 0, 0, 1, 60);
        addAssignment(s, f, 0, "Pilot");
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkill>();
        checkScore("requiredSkill satisfied", scoreWith(calculator, s), 0, 0.0);
    }
    {  // flightConflict alone: two overlapping flights on one employee.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 1, 0, 11), 0);
        DecomposedCalculator calculator;
        calculator.emplace<FlightConflict>();
        checkScore("flightConflict alone", scoreWith(calculator, s), -10, 0.0);
    }
    {  // Back to back, not overlapping: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 20), 0);
        DecomposedCalculator calculator;
        calculator.emplace<FlightConflict>();
        checkScore(
            "flightConflict, back to back", scoreWith(calculator, s), 0, 0.0);
    }
    {  // transferBetweenTwoFlights alone: lands at airport 1, next leaves
        // from airport 0.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 0, 20, 0, 30), 0);
        DecomposedCalculator calculator;
        calculator.emplace<TransferBetweenTwoFlights>();
        checkScore("transferBetweenTwoFlights alone",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {  // Continuing from the arrival airport: silent.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        DecomposedCalculator calculator;
        calculator.emplace<TransferBetweenTwoFlights>();
        checkScore("transferBetweenTwoFlights, connected",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // employeeUnavailability alone: a day-0 flight on an employee who is
        // unavailable that day.
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {0};
        addAssignment(s, addFlight(s, 0, 0, 1, 60), 0);
        DecomposedCalculator calculator;
        calculator.emplace<EmployeeUnavailability>();
        checkScore(
            "employeeUnavailability alone", scoreWith(calculator, s), -10, 0.0);
    }
    {  // The two home-airport rules: the single flight is both first and
        // last, leaving from airport 1 (home is 0) and landing at 1.
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 1, 0, 1, 60), 0);
        DecomposedCalculator first;
        first.emplace<FirstAssignmentNotDepartingFromHome>();
        checkScore("firstAssignmentNotDepartingFromHome alone",
                   scoreWith(first, s),
                   0,
                   -1000.0);
        DecomposedCalculator last;
        last.emplace<LastAssignmentNotArrivingAtHome>();
        checkScore("lastAssignmentNotArrivingAtHome alone",
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
        calculator.emplace<RequiredSkill>(1.0);
        checkScore("requiredSkill re-weighted to 1",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredSkill>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("requiredSkill demoted to soft",
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
        auto component = std::make_unique<RequiredSkill>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("requiredSkill with exponent 2",
                   scoreWith(calculator, two),
                   -400,
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

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All flight-crew-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d flight-crew-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}
