// pybind11 bindings for the flight-crew-scheduling C++ domain wrappers.
// The Python extension links the existing fcs::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::set / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fcs/constraints.hpp"
#include "fcs/domain.hpp"
#include "fcs/score_calculator.hpp"
#include "fcs/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace fcs;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Flight crew scheduling — Timefold engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Airport>(m, "Airport", py::module_local())
        .def(py::init(
                 [](std::string code,
                    std::string name,
                    double latitude,
                    double longitude)
                 {
                     Airport a;
                     a.code = std::move(code);
                     a.name = std::move(name);
                     a.latitude = latitude;
                     a.longitude = longitude;
                     return a;
                 }),
             py::arg("code"),
             py::arg("name") = "",
             py::arg("latitude") = 0.0,
             py::arg("longitude") = 0.0)
        .def_readwrite("code", &Airport::code)
        .def_readwrite("name", &Airport::name)
        .def_readwrite("latitude", &Airport::latitude)
        .def_readwrite("longitude", &Airport::longitude);

    py::class_<Employee>(m, "Employee", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    int homeAirport,
                    std::vector<std::string> skills,
                    std::set<DayIndex> unavailableDays)
                 {
                     Employee e;
                     e.id = std::move(id);
                     e.name = std::move(name);
                     e.homeAirport = homeAirport;
                     e.skills = std::move(skills);
                     e.unavailableDays = std::move(unavailableDays);
                     return e;
                 }),
             py::arg("id"),
             py::arg("name") = "",
             py::arg("home_airport") = -1,
             py::arg("skills") = std::vector<std::string>{},
             py::arg("unavailable_days") = std::set<DayIndex>{})
        .def_readwrite("id", &Employee::id)
        .def_readwrite("name", &Employee::name)
        // Index into FlightCrewSchedule.airports.
        .def_readwrite("home_airport", &Employee::homeAirport)
        .def_readwrite("skills", &Employee::skills)
        // Calendar-day indices (floor(minutes / 1440)).
        .def_readwrite("unavailable_days", &Employee::unavailableDays);

    py::class_<Flight>(m, "Flight", py::module_local())
        .def(py::init(
                 [](std::string flightNumber,
                    int departureAirport,
                    Minutes departureUTC,
                    int arrivalAirport,
                    Minutes arrivalUTC)
                 {
                     Flight f;
                     f.flightNumber = std::move(flightNumber);
                     f.departureAirport = departureAirport;
                     f.departureUTC = departureUTC;
                     f.arrivalAirport = arrivalAirport;
                     f.arrivalUTC = arrivalUTC;
                     return f;
                 }),
             py::arg("flight_number"),
             py::arg("departure_airport"),
             py::arg("departure_utc"),
             py::arg("arrival_airport"),
             py::arg("arrival_utc"))
        .def_readwrite("flight_number", &Flight::flightNumber)
        .def_readwrite("departure_airport", &Flight::departureAirport)
        // Minutes from the reference epoch (the Python models use Unix).
        .def_readwrite("departure_utc", &Flight::departureUTC)
        .def_readwrite("arrival_airport", &Flight::arrivalAirport)
        .def_readwrite("arrival_utc", &Flight::arrivalUTC);

    py::class_<FlightAssignment>(m, "FlightAssignment", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int flight,
                    int indexInFlight,
                    std::string requiredSkill,
                    int employee,
                    bool pinned)
                 {
                     FlightAssignment a;
                     a.id = std::move(id);
                     a.flight = flight;
                     a.indexInFlight = indexInFlight;
                     a.requiredSkill = std::move(requiredSkill);
                     a.employee = employee;
                     a.pinned = pinned;
                     return a;
                 }),
             py::arg("id"),
             py::arg("flight"),
             py::arg("index_in_flight") = 0,
             py::arg("required_skill") = "",
             py::arg("employee") = -1,
             py::arg("pinned") = false)
        .def_readwrite("id", &FlightAssignment::id)
        .def_readwrite("flight", &FlightAssignment::flight)
        .def_readwrite("index_in_flight", &FlightAssignment::indexInFlight)
        .def_readwrite("required_skill", &FlightAssignment::requiredSkill)
        // The planning variable: index into employees; -1 == unassigned.
        .def_readwrite("employee", &FlightAssignment::employee)
        .def_readwrite("pinned", &FlightAssignment::pinned);

    py::class_<FlightCrewSchedule>(m, "FlightCrewSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Airport> airports,
                    std::vector<Employee> employees,
                    std::vector<Flight> flights,
                    std::vector<FlightAssignment> flightAssignments)
                 {
                     FlightCrewSchedule s;
                     s.airports = std::move(airports);
                     s.employees = std::move(employees);
                     s.flights = std::move(flights);
                     s.flightAssignments = std::move(flightAssignments);
                     return s;
                 }),
             py::arg("airports"),
             py::arg("employees"),
             py::arg("flights"),
             py::arg("flight_assignments"))
        .def_readwrite("airports", &FlightCrewSchedule::airports)
        .def_readwrite("employees", &FlightCrewSchedule::employees)
        .def_readwrite("flights", &FlightCrewSchedule::flights)
        // NOTE: element access returns copies; use set_assignment for bulk
        // employee writes.
        .def_readwrite("flight_assignments",
                       &FlightCrewSchedule::flightAssignments)
        .def_readonly("score", &FlightCrewSchedule::score)
        .def_readonly("prepared", &FlightCrewSchedule::prepared)
        // Convenience: the current employee per assignment (the solution).
        .def("assignment",
             [](const FlightCrewSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.flightAssignments.size());
                 for (const FlightAssignment &assignment : s.flightAssignments)
                     a.push_back(assignment.employee);
                 return a;
             })
        // Bulk write of the planning variable (assignment access via
        // .flight_assignments returns copies, so per-item writes would be
        // lost).
        .def(
            "set_assignment",
            [](FlightCrewSchedule &s, const std::vector<int> &a)
            {
                if (a.size() != s.flightAssignments.size())
                    throw py::value_error(
                        "assignment length must equal the flight-assignment "
                        "count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    s.flightAssignments[i].employee = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");

    py::class_<Solver::Config>(m, "SolverConfig", py::module_local())
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Solver::Config &c, double s)
            {
                c.spentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Solver::Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "unimproved_spent_limit_seconds must be non-negative");
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::unimprovedStepCountLimit)
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
        .def_property(
            "parallel_solver_count",
            [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error(
                        "parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // 0 = auto: clamp(4 * assignment count, 400, 3200).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.lateAcceptanceSize; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error(
                        "late_acceptance_size must be >= 0 (0 = auto)");
                c.lateAcceptanceSize = static_cast<std::size_t>(size);
            })
        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c)
            {
                return c.environmentMode
                       == optimaru::EnvironmentMode::FULL_ASSERT;
            },
            [](Solver::Config &c, bool on)
            {
                c.environmentMode = on ? optimaru::EnvironmentMode::FULL_ASSERT
                                       : optimaru::EnvironmentMode::NON_ASSERT;
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruinRecreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger",
                       &Solver::Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction",
                       &Solver::Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction",
                       &Solver::Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count",
                       &Solver::Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_operator_decay",
                       &Solver::Config::ruinRecreateOperatorDecay)
        .def_property(
            "ruin_recreate_rewards",
            [](const Solver::Config &c) { return c.ruinRecreateRewards; },
            [](Solver::Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error(
                        "ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -1e18)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.bestScoreLimit = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets flight_assignments[*].employee and
        // schedule.score. The GIL is released so parallel multi-start workers
        // can fire on_new_best.
        .def("solve",
             py::overload_cast<FlightCrewSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // flight_crew.constraints). The picks replace the defaults; None keeps
        // them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               FlightCrewSchedule &schedule,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `schedule` — the input
    // flight_crew.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](FlightCrewSchedule &schedule)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            return tfpy::constraintCosts(calculator, schedule);
        },
        py::arg("schedule"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraintNames(defaultConstraints()); },
        "Constraint names available to flight_crew.constraints.");
}
