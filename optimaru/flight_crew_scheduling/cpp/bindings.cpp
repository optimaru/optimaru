// pybind11 bindings for the flight-crew-scheduling C++ domain wrappers.
// The Python extension links the existing fcs::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
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
    m.doc() = "Flight crew scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

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
        // The decision variable: index into employees; -1 == unassigned.
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
        // Bulk write of the decision variable (assignment access via
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
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");

    optimaru::python::bind_solver_config(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
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
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.target_score.hard,
                                      c.target_score.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.target_score = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

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
        // optimaru.flight_crew_scheduling.constraints). The picks replace the defaults; None keeps
        // them. A name may be picked several times: every pick is its own
        // instance (a fresh copy of the default component) with its own
        // knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               FlightCrewSchedule &schedule,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); },
                                       constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `schedule` — the input
    // optimaru.flight_crew_scheduling.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](FlightCrewSchedule &schedule, const py::object &constraints)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, schedule);
        },
        py::arg("schedule"),
        py::arg("constraints") = py::none(),
        "Raw per-employee cost of every constraint, before "
        "weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.flight_crew_scheduling.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
