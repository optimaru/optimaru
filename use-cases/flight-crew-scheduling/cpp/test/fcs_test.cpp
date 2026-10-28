// Flight-crew-scheduling unit tests (engine test/ style: check() + non-zero exit
// on any failure):
//   (a) every constraint verified in isolation against hand-computed scores
//       (including the Java ConstraintProviderTest cases)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) pinned assignments keep their employee through a solve
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "fcs/domain.hpp"
#include "fcs/incremental_calculator.hpp"
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
                long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-46s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// Base problem: `airportCount` airports and `employees` crew members, all
// with the given skill and home airport 0 unless changed by the caller.
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

// -------------------------------------------------------------------------
// (a) Each constraint in isolation, full recompute vs hand-computed scores.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: requiredSkill — the Java requiredSkill test (skill mismatch).
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].skills = {"2"};
        const int f = addFlight(s, 0, 0, 1, 60);
        addAssignment(s, f, 0, "1");
        checkScore("requiredSkill missing",
                   ScoreCalculator::calculateScore(s),
                   -100,
                   -1000);  // last assignment arrives at 1 != home 0
    }
    {
        FlightCrewSchedule s = makeBase(2, 1);
        const int f = addFlight(s, 0, 0, 0, 60);
        addAssignment(s, f, 0, "Pilot");
        checkScore("requiredSkill held (home round trip)",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }

    // hard 2: flightConflict — the Java flightConflict test: [0,10) and
    // [1,11) overlap -> one pair (-10). The pair is also a departure-
    // consecutive pair, but its connection (arr airport 0 -> dep airport 0)
    // holds, and both flights start/end at home, so nothing else fires.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 1, 0, 11), 0);
        checkScore("flightConflict overlapping pair",
                   ScoreCalculator::calculateScore(s),
                   -10,
                   0);
    }
    // Three mutually overlapping flights -> C(3,2) = 3 pairs.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 100), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 100), 0);
        addAssignment(s, addFlight(s, 0, 20, 0, 100), 0);
        // transfers: groups {0},{1},{2} consecutive; arr airport 0 == dep 0.
        checkScore("flightConflict triple",
                   ScoreCalculator::calculateScore(s),
                   -30,
                   0);
    }
    // Touching intervals ([0,10) then [10,20)) do NOT overlap.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 10, 0, 20), 0);
        checkScore("flightConflict touching = none",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }
    // Different employees never conflict.
    {
        FlightCrewSchedule s = makeBase(2, 2);
        addAssignment(s, addFlight(s, 0, 0, 0, 10), 0);
        addAssignment(s, addFlight(s, 0, 1, 0, 11), 1);
        checkScore("flightConflict different employees",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }

    // hard 3: transfer — the Java transferBetweenTwoFlights test: three
    // flights 0->1 at deps 0, 11, 13; consecutive pairs (0,11) and (11,13)
    // both break the connection (arr airport 1 != dep airport 0) -> -2.
    // Flights: [0,10), [11,12), [13,14): no overlap. Soft: first departs 0
    // (home) -> 0; last arrives 1 != home -> -1000.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 0, 11, 1, 12), 0);
        addAssignment(s, addFlight(s, 0, 13, 1, 14), 0);
        checkScore("transfer two broken connections (Java case)",
                   ScoreCalculator::calculateScore(s),
                   -2,
                   -1000);
    }
    // Connected chain 0->1, 1->0: no transfer penalty, home round trip.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 1, 20, 0, 30), 0);
        checkScore("transfer connected chain",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }
    // Departure-time tie on the LATER side: a alone at dep 0, b and c tied
    // at dep 20 -> both (a,b) and (a,c) are consecutive pairs; both broken.
    // b,c overlap each other ([20,30) x [20,30)) -> -10. b and c arrive at
    // home (airport 0) but the tie means BOTH are also last assignments
    // (arriving home -> no soft); first departs home.
    {
        FlightCrewSchedule s = makeBase(3, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 2, 20, 0, 30), 0);
        addAssignment(s, addFlight(s, 2, 20, 0, 30), 0);
        checkScore("transfer tie on second group (2 pairs + conflict)",
                   ScoreCalculator::calculateScore(s),
                   -2 - 10,
                   0);
    }
    // Departure-time tie on the EARLIER side: a and b tied at dep 0 mean
    // neither starts a consecutive pair (the Java ifNotExists matches the
    // other tied assignment) -> no transfer penalty even though the
    // connection to c (dep 20) is broken. a,b overlap -> -10. Ties: both a
    // and b are first assignments departing from home; c is the last,
    // arriving home.
    {
        FlightCrewSchedule s = makeBase(3, 1);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 0, 0, 1, 10), 0);
        addAssignment(s, addFlight(s, 2, 20, 0, 30), 0);
        checkScore("transfer tie on first group suppresses pair",
                   ScoreCalculator::calculateScore(s),
                   -10,
                   0);
    }

    // hard 4: employeeUnavailability — the Java employeeUnavailability test.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {5};
        // departs on the unavailable day (day 5), home round trip
        addAssignment(
            s,
            addFlight(s, 0, 5 * MINUTES_PER_DAY, 0, 5 * MINUTES_PER_DAY + 10),
            0);
        checkScore("unavailable at departure",
                   ScoreCalculator::calculateScore(s),
                   -10,
                   0);
    }
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {5};
        // departs day 4, arrives day 5: unavailable DURING the flight
        addAssignment(
            s,
            addFlight(s, 0, 4 * MINUTES_PER_DAY, 0, 5 * MINUTES_PER_DAY + 10),
            0);
        checkScore("unavailable during flight",
                   ScoreCalculator::calculateScore(s),
                   -10,
                   0);
    }
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {5};
        // flies a day later: available
        addAssignment(
            s,
            addFlight(s, 0, 6 * MINUTES_PER_DAY, 0, 6 * MINUTES_PER_DAY + 10),
            0);
        checkScore("available day", ScoreCalculator::calculateScore(s), 0, 0);
    }

    // soft 5/6: the Java firstAssignmentNotDepartingFromHome test. Employee 0
    // (home airport 0) flies 1->2 at deps 0, 1, 1; employee 1 (home 2) flies
    // 2->3 at dep 0. Isolate the FIRST constraint by keeping every arrival
    // "home-looking" impossible — instead assert the combined soft exactly:
    //   employee 0: first = dep-0 assignment (alone), departs 1 != 0 -> -1000
    //               last  = the TWO dep-1 assignments, arrive 2 != 0 -> -2000
    //               conflicts: [0,10) x [1,10) x [1,10): 3 pairs -> -30
    //               transfers: group {0} then group {1,1}: arr 2 != dep 1
    //               twice -> -2
    //   employee 1: single flight 2->3: first departs 2 == home -> 0;
    //               last arrives 3 != 2 -> -1000
    {
        FlightCrewSchedule s = makeBase(4, 2);
        s.employees[0].homeAirport = 0;
        s.employees[1].homeAirport = 2;
        addAssignment(s, addFlight(s, 1, 0, 2, 10), 0);
        addAssignment(s, addFlight(s, 1, 1, 2, 10), 0);
        addAssignment(s, addFlight(s, 1, 1, 2, 10), 0);
        addAssignment(s, addFlight(s, 2, 0, 3, 10), 1);
        checkScore("first/last not home (Java case, combined)",
                   ScoreCalculator::calculateScore(s),
                   -32,
                   -4000);
    }
    // soft 6 isolated: the Java lastAssignmentNotArrivingAtHome test.
    // Employee 0 (home 0): 1->2 [0,10), 2->3 [11,12): connected, no overlap.
    // first departs 1 != 0 -> -1000; last arrives 3 != 0 -> -1000.
    // Employee 1 (home 1): 1->2 [0,10), 2->1 [11,12): first departs home,
    // last arrives home -> 0.
    {
        FlightCrewSchedule s = makeBase(4, 2);
        s.employees[0].homeAirport = 0;
        s.employees[1].homeAirport = 1;
        addAssignment(s, addFlight(s, 1, 0, 2, 10), 0);
        addAssignment(s, addFlight(s, 2, 11, 3, 12), 0);
        addAssignment(s, addFlight(s, 1, 0, 2, 10), 1);
        addAssignment(s, addFlight(s, 2, 11, 1, 12), 1);
        checkScore("last not arriving home (Java case)",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -2000);
    }

    // Unassigned assignments contribute nothing.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].skills = {"2"};
        const int f = addFlight(s, 1, 0, 1, 10);
        addAssignment(s, f, -1, "1");
        checkScore("unassigned contributes nothing",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }
    // Pinned assignments are scored like any other.
    {
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].skills = {"2"};
        const int f = addFlight(s, 0, 0, 0, 10);
        addAssignment(s, f, 0, "1");
        s.flightAssignments.back().pinned = true;
        checkScore("pinned assignment still scored",
                   ScoreCalculator::calculateScore(s),
                   -100,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 18, /*employees*/ 8, /*seed*/ 7);

    FlightCrewIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<FlightCrewSchedule> director(s, calc);
    FlightCrewSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int assignmentCount = static_cast<int>(s.flightAssignments.size());
    const int employeeCount = static_cast<int>(s.employees.size());
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % employeeCount);
            director.changeVariable(assignment, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.flightAssignments[a].employee;
            const int vb = s.flightAssignments[b].employee;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < assignmentCount; ++i)
        {
            shadow.flightAssignments[i].employee
                = s.flightAssignments[i].employee;
        }
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
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
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 12, /*employees*/ 6, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(s);
        final = s.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-9,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinned assignments keep their employee through a solve, and every
// assignment ends up assigned.
// -------------------------------------------------------------------------
void verifyPinnedSolve()
{
    std::printf("== (d) Pinned assignment survives a solve ==\n");
    FlightCrewSchedule s
        = buildRandomInstance(/*flights*/ 8, /*employees*/ 6, /*seed*/ 21);
    s.flightAssignments[0].employee = 3;
    s.flightAssignments[0].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(s);

    check(s.flightAssignments[0].employee == 3,
          "pinned assignment kept its employee");
    bool allAssigned = true;
    for (const FlightAssignment &assignment : s.flightAssignments)
    {
        if (assignment.employee < 0)
            allAssigned = false;
    }
    check(allAssigned, "every assignment has an employee after the solve");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedSolve();

    if (failures == 0)
    {
        std::printf("All flight-crew-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d flight-crew-scheduling check(s) FAILED.\n", failures);
    return 1;
}
