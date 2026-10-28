// Flight-crew-scheduling demo: reproduces the Java DemoDataGenerator's
// instance shape (6 airports, 5 days, 14 flights in home-based round trips,
// 2 pilots + 2-3 attendants per flight, two teams of crew per departure
// airport, 28%/4% unavailability), solves for N seconds and asserts a
// feasible (hard 0) result.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "fcs/domain.hpp"
#include "fcs/score_calculator.hpp"
#include "fcs/solver.hpp"

using namespace fcs;

namespace
{

constexpr const char *PILOT_SKILL = "Pilot";
constexpr const char *ATTENDANT_SKILL = "Flight attendant";

const std::vector<std::string> FIRST_NAMES
    = {"Amy",  "Beth", "Carl", "Dan",  "Elsa", "Flo",  "Gus",  "Hugo", "Ivy",
       "Jay",  "Jeri", "Hope", "Avis", "Lino", "Lyle", "Nick", "Dino", "Otha",
       "Gwen", "Jose", "Dena", "Jana", "Dave", "Russ", "Josh", "Dana", "Katy"};
const std::vector<std::string> LAST_NAMES
    = {"Cole", "Fox",   "Green", "Jones", "King", "Li",   "Poe",
       "Rye",  "Smith", "Watt",  "Howe",  "Lowe", "Wise", "Clay",
       "Carr", "Hood",  "Long",  "Horn",  "Haas", "Meza"};

// Same 6 airports and pairwise flight durations (hours) as the Java demo.
const std::vector<Airport> AIRPORTS = {
    {"LHR", "LHR", 51.4775, -0.461389},
    {"JFK", "JFK", 40.639722, -73.778889},
    {"CNF", "CNF", -19.624444, -43.971944},
    {"BRU", "BRU", 50.901389, 4.484444},
    {"ATL", "ATL", 33.636667, -84.428056},
    {"BNE", "BNE", -27.383333, 153.118333},
};
// hours[from][to], airport order as above; 0 on the diagonal (unused).
constexpr int HOURS[6][6] = {
    {0, 8, 12, 13, 9, 21},
    {8, 0, 10, 14, 6, 20},
    {12, 10, 0, 19, 10, 19},
    {13, 14, 19, 0, 9, 21},
    {9, 6, 10, 9, 0, 18},
    {21, 20, 19, 21, 18, 0},
};

constexpr int COUNT_DAYS = 5;
constexpr int COUNT_FLIGHTS = 14;

int pickDifferent(std::mt19937_64 &rng, int size, int exclude)
{
    int pick = exclude;
    while (pick == exclude)
        pick = static_cast<int>(rng() % size);
    return pick;
}

FlightCrewSchedule generateDemoData(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    FlightCrewSchedule schedule;
    schedule.airports = AIRPORTS;
    const int airportCount = static_cast<int>(AIRPORTS.size());

    // Two distinct home airports; the rest are route waypoints.
    const int home0 = static_cast<int>(rng() % airportCount);
    const int home1 = pickDifferent(rng, airportCount, home0);
    std::vector<int> remainingAirports;
    for (int a = 0; a < airportCount; ++a)
    {
        if (a != home0 && a != home1)
            remainingAirports.push_back(a);
    }

    // Routes of 2/4/6 flights leaving from and returning to a home airport
    // (the Java generateFlights loop).
    struct Route
    {
        int departure;
        int arrival;
    };
    std::vector<Route> routes;
    while (static_cast<int>(routes.size()) < COUNT_FLIGHTS)
    {
        const int limit = COUNT_FLIGHTS - static_cast<int>(routes.size());
        int routeSize = 0;
        const int allowed[3] = {2, 4, 6};
        while (routeSize == 0 || routeSize > limit)
            routeSize = allowed[rng() % 3];
        const int home = (rng() % 2 == 0) ? home0 : home1;
        int current = remainingAirports[rng() % remainingAirports.size()];
        routes.push_back({home, current});
        for (int i = 0; i < routeSize - 2; ++i)
        {
            int nextIndex = static_cast<int>(rng() % remainingAirports.size());
            while (remainingAirports[nextIndex] == current)
                nextIndex = static_cast<int>(rng() % remainingAirports.size());
            routes.push_back({current, remainingAirports[nextIndex]});
            current = remainingAirports[nextIndex];
        }
        routes.push_back({current, home});
    }

    // Departure/arrival times: spread the flights over the 5 days (2-3 per
    // day), departures on the hour 00..22 (the Java time groups), duration
    // from the distance table. Day 0 minute 1 plays LocalDateTime.now(): any
    // draw before it is pushed to now + rand(4) hours.
    const Minutes startDateTime = 1;
    std::vector<int> flightDay(COUNT_FLIGHTS);
    for (int f = 0; f < COUNT_FLIGHTS; ++f)
        flightDay[f] = f % COUNT_DAYS;
    std::shuffle(flightDay.begin(), flightDay.end(), rng);

    for (int f = 0; f < COUNT_FLIGHTS; ++f)
    {
        Flight flight;
        flight.flightNumber = "Flight " + std::to_string(f + 1);
        flight.departureAirport = routes[f].departure;
        flight.arrivalAirport = routes[f].arrival;
        Minutes departure = flightDay[f] * MINUTES_PER_DAY
                            + static_cast<Minutes>(rng() % 23) * 60;
        if (departure < startDateTime)
            departure = startDateTime + static_cast<Minutes>(rng() % 4) * 60;
        flight.departureUTC = departure;
        flight.arrivalUTC
            = departure + 60 * HOURS[routes[f].departure][routes[f].arrival];
        schedule.flights.push_back(flight);
    }

    // Flight assignments: 2 pilots + 2 attendants, plus a 3rd attendant on
    // CNF flights (the Java generateFlightAssignments).
    int assignmentId = 0;
    for (int f = 0; f < COUNT_FLIGHTS; ++f)
    {
        int indexInFlight = 0;
        const auto addAssignment = [&](const char *skill)
        {
            FlightAssignment assignment;
            assignment.id = std::to_string(++assignmentId);
            assignment.flight = f;
            assignment.indexInFlight = ++indexInFlight;
            assignment.requiredSkill = skill;
            schedule.flightAssignments.push_back(assignment);
        };
        addAssignment(PILOT_SKILL);
        addAssignment(PILOT_SKILL);
        addAssignment(ATTENDANT_SKILL);
        addAssignment(ATTENDANT_SKILL);
        const bool touchesCnf = AIRPORTS[routes[f].departure].code == "CNF"
                                || AIRPORTS[routes[f].arrival].code == "CNF";
        if (touchesCnf)
            addAssignment(ATTENDANT_SKILL);
    }

    // Employees: two teams (2 pilots + 2 attendants, +1 attendant at CNF)
    // per distinct flight departure airport (the Java generateEmployees).
    std::vector<int> flightAirports;
    for (int f = 0; f < COUNT_FLIGHTS; ++f)
    {
        const int airport = routes[f].departure;
        bool seen = false;
        for (const int existing : flightAirports)
            seen = seen || existing == airport;
        if (!seen)
            flightAirports.push_back(airport);
    }
    int employeeId = 0;
    const auto addEmployee = [&](int airport, const char *skill)
    {
        Employee employee;
        employee.id = std::to_string(++employeeId);
        employee.name = FIRST_NAMES[rng() % FIRST_NAMES.size()] + " "
                        + LAST_NAMES[rng() % LAST_NAMES.size()];
        employee.homeAirport = airport;
        employee.skills = {skill};
        schedule.employees.push_back(employee);
    };
    for (const int airport : flightAirports)
    {
        for (int team = 0; team < 2; ++team)
        {
            addEmployee(airport, PILOT_SKILL);
            addEmployee(airport, PILOT_SKILL);
            addEmployee(airport, ATTENDANT_SKILL);
            addEmployee(airport, ATTENDANT_SKILL);
            if (AIRPORTS[airport].code == "CNF")
                addEmployee(airport, ATTENDANT_SKILL);
        }
    }

    // Unavailable days: 28% of the crew one day, another 4% two days.
    const int employeeCount = static_cast<int>(schedule.employees.size());
    std::vector<int> shuffled(employeeCount);
    for (int e = 0; e < employeeCount; ++e)
        shuffled[e] = e;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    const int oneDay = static_cast<int>(0.28 * employeeCount);
    const int twoDays = static_cast<int>(0.04 * employeeCount);
    for (int k = 0; k < oneDay + twoDays && k < employeeCount; ++k)
    {
        Employee &employee = schedule.employees[shuffled[k]];
        employee.unavailableDays.insert(
            static_cast<DayIndex>(rng() % COUNT_DAYS));
        while (k >= oneDay && employee.unavailableDays.size() < 2)
        {
            employee.unavailableDays.insert(
                static_cast<DayIndex>(rng() % COUNT_DAYS));
        }
    }
    return schedule;
}

void printSchedule(const FlightCrewSchedule &schedule)
{
    std::printf("  [flights & crew]\n");
    for (int f = 0; f < static_cast<int>(schedule.flights.size()); ++f)
    {
        const Flight &flight = schedule.flights[f];
        std::printf(
            "    %-9s %s d%lld %02lld:%02lld -> %s d%lld %02lld:%02lld :",
            flight.flightNumber.c_str(),
            schedule.airports[flight.departureAirport].code.c_str(),
            static_cast<long long>(dayOf(flight.departureUTC)),
            static_cast<long long>(flight.departureUTC / 60 % 24),
            static_cast<long long>(flight.departureUTC % 60),
            schedule.airports[flight.arrivalAirport].code.c_str(),
            static_cast<long long>(dayOf(flight.arrivalUTC)),
            static_cast<long long>(flight.arrivalUTC / 60 % 24),
            static_cast<long long>(flight.arrivalUTC % 60));
        for (const FlightAssignment &assignment : schedule.flightAssignments)
        {
            if (assignment.flight != f)
                continue;
            if (assignment.employee < 0)
            {
                std::printf("  (미배정)");
                continue;
            }
            const Employee &employee = schedule.employees[assignment.employee];
            std::printf("  %s(%s)",
                        employee.name.c_str(),
                        assignment.requiredSkill == PILOT_SKILL ? "P" : "A");
        }
        std::printf("\n");
    }

    std::printf("\n  [per-employee itineraries]\n");
    for (int e = 0; e < static_cast<int>(schedule.employees.size()); ++e)
    {
        std::vector<int> itinerary;
        for (int i = 0; i < static_cast<int>(schedule.flightAssignments.size());
             ++i)
        {
            if (schedule.flightAssignments[i].employee == e)
                itinerary.push_back(i);
        }
        if (itinerary.empty())
            continue;
        ScoreCalculator::sortByDeparture(schedule, itinerary);
        const Employee &employee = schedule.employees[e];
        std::printf("    %-12s (home %s):",
                    employee.name.c_str(),
                    schedule.airports[employee.homeAirport].code.c_str());
        for (const int i : itinerary)
        {
            const Flight &flight
                = schedule.flights[schedule.flightAssignments[i].flight];
            std::printf(" %s->%s",
                        schedule.airports[flight.departureAirport].code.c_str(),
                        schedule.airports[flight.arrivalAirport].code.c_str());
        }
        std::printf("\n");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    FlightCrewSchedule schedule = generateDemoData(/*seed*/ 0);
    std::printf("== 항공 승무원 스케줄링 데모 (공항 %zu, 항공편 %zu, 좌석 %zu, "
                "승무원 %zu, %d초) ==\n",
                schedule.airports.size(),
                schedule.flights.size(),
                schedule.flightAssignments.size(),
                schedule.employees.size(),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(schedule);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                schedule.score.toString().c_str(),
                schedule.score.isFeasible() ? "true" : "false");
    printSchedule(schedule);

    bool allAssigned = true;
    for (const FlightAssignment &assignment : schedule.flightAssignments)
        allAssigned = allAssigned && assignment.employee >= 0;
    std::printf("  전 좌석 배정: %s\n", allAssigned ? "OK" : "FAIL");

    if (schedule.score.hard == 0 && allAssigned)
    {
        std::printf("  hard 0 도달: OK\n");
        return 0;
    }
    std::printf("  hard 0 도달 실패 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}
