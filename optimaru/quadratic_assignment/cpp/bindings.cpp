// pybind11 bindings for the quadratic-assignment C++ domain.
// The Python extension links the qap::* implementations and imports the
// generic optimaru engine from optimaru/cpp/optimaru; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::optional conversions

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"
#include "qap/constraints.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"
#include "qap/solver.hpp"

namespace py = pybind11;
using namespace qap;
namespace opy = optimaru::python;

namespace {

using Matrix = std::vector<std::vector<std::int64_t>>;

// A fact setter: write the value and drop the prepared caches, so the next
// score / solve re-validates and rebuilds them.
template <typename T, T QuadraticAssignmentProblem::* Member>
void setFact(QuadraticAssignmentProblem& problem, T value) {
    problem.*Member = std::move(value);
    problem.prepared = false;
}

// Validated facts, then a read (the totals index the matrices directly).
QuadraticAssignmentProblem& prepared(QuadraticAssignmentProblem& problem) {
    ScoreCalculator::prepare(problem);
    return problem;
}

void setAssignment(QuadraticAssignmentProblem& problem, const std::vector<int>& assignment) {
    if (assignment.size() != problem.locations.size())
        throw py::value_error("assignment length must equal the location count");
    std::vector<int> locationOf(problem.facilities.size(), -1);
    for (std::size_t l = 0; l < assignment.size(); ++l) {
        const int f = assignment[l];
        if (f == -1) continue;
        if (f < -1 || f >= problem.facilityCount()) {
            throw py::value_error("location '" + problem.locations[l].id + "': facility index " +
                                  std::to_string(f) + " out of range (-1 = empty)");
        }
        if (locationOf[f] >= 0) {
            throw py::value_error("facility '" + problem.facilities[f].id +
                                  "' is assigned to both '" + problem.locations[locationOf[f]].id +
                                  "' and '" + problem.locations[l].id + "'");
        }
        locationOf[f] = static_cast<int>(l);
    }
    for (std::size_t f = 0; f < problem.facilities.size(); ++f)
        problem.facilities[f].location = locationOf[f];
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Quadratic assignment — optimaru engine-backed C++ wrapper exposed to Python";

    // Every class is module_local: the server's isolated runtime loads several
    // domains whose classes share names (Score, Settings, Solver).
    opy::bind_score<Score>(m);

    py::class_<Facility>(m, "Facility", py::module_local())
            .def(py::init([](std::string id, std::optional<std::int64_t> demand, int location) {
                     return Facility{std::move(id), demand, location};
                 }),
                 py::arg("id"), py::arg("demand") = py::none(), py::arg("location") = -1)
            .def_readwrite("id", &Facility::id)
            // facility_demands[id]; None = not listed (scored as demand 0).
            .def_readwrite("demand", &Facility::demand)
            // The decision variable: index into problem.locations; -1 == unassigned.
            .def_readwrite("location", &Facility::location)
            .def("is_assigned", &Facility::isAssigned);

    py::class_<Location>(m, "Location", py::module_local())
            .def(py::init([](std::string id, std::optional<std::int64_t> capacity) {
                     return Location{std::move(id), capacity};
                 }),
                 py::arg("id"), py::arg("capacity") = py::none())
            .def_readwrite("id", &Location::id)
            // location_capacities[id]; None = no capacity limit.
            .def_readwrite("capacity", &Location::capacity);

    py::class_<FixedAssignment>(m, "FixedAssignment", py::module_local())
            .def(py::init([](int location, int facility) {
                     return FixedAssignment{location, facility};
                 }),
                 py::arg("location"), py::arg("facility"))
            .def_readwrite("location", &FixedAssignment::location)
            .def_readwrite("facility", &FixedAssignment::facility);

    py::class_<ForbiddenAssignment>(m, "ForbiddenAssignment", py::module_local())
            .def(py::init([](int facility, int location) {
                     return ForbiddenAssignment{facility, location};
                 }),
                 py::arg("facility"), py::arg("location"))
            .def_readwrite("facility", &ForbiddenAssignment::facility)
            .def_readwrite("location", &ForbiddenAssignment::location);

    py::class_<QuadraticAssignmentProblem>(m, "QuadraticAssignmentProblem", py::module_local())
            .def(py::init([](std::vector<Facility> facilities, std::vector<Location> locations,
                             Matrix flows, Matrix distances, std::optional<Matrix> assignmentCosts,
                             std::vector<FixedAssignment> fixedAssignments,
                             std::vector<ForbiddenAssignment> forbiddenAssignments,
                             bool warmStart) {
                     QuadraticAssignmentProblem problem;
                     problem.facilities = std::move(facilities);
                     problem.locations = std::move(locations);
                     problem.flows = std::move(flows);
                     problem.distances = std::move(distances);
                     problem.assignmentCosts = std::move(assignmentCosts);
                     problem.fixedAssignments = std::move(fixedAssignments);
                     problem.forbiddenAssignments = std::move(forbiddenAssignments);
                     problem.warmStart = warmStart;
                     return problem;
                 }),
                 py::arg("facilities"), py::arg("locations"), py::arg("flows"),
                 py::arg("distances"), py::arg("assignment_costs") = py::none(),
                 py::arg("fixed_assignments") = std::vector<FixedAssignment>{},
                 py::arg("forbidden_assignments") = std::vector<ForbiddenAssignment>{},
                 py::arg("warm_start") = false)
            // NOTE: element access returns copies; use set_assignment for bulk
            // location writes. Assigning a fact drops the prepared caches.
            .def_property(
                    "facilities", [](const QuadraticAssignmentProblem& p) { return p.facilities; },
                    &setFact<std::vector<Facility>, &QuadraticAssignmentProblem::facilities>)
            .def_property(
                    "locations", [](const QuadraticAssignmentProblem& p) { return p.locations; },
                    &setFact<std::vector<Location>, &QuadraticAssignmentProblem::locations>)
            .def_property(
                    "flows", [](const QuadraticAssignmentProblem& p) { return p.flows; },
                    &setFact<Matrix, &QuadraticAssignmentProblem::flows>)
            .def_property(
                    "distances", [](const QuadraticAssignmentProblem& p) { return p.distances; },
                    &setFact<Matrix, &QuadraticAssignmentProblem::distances>)
            .def_property(
                    "assignment_costs",
                    [](const QuadraticAssignmentProblem& p) { return p.assignmentCosts; },
                    &setFact<std::optional<Matrix>, &QuadraticAssignmentProblem::assignmentCosts>)
            .def_property(
                    "fixed_assignments",
                    [](const QuadraticAssignmentProblem& p) { return p.fixedAssignments; },
                    &setFact<std::vector<FixedAssignment>,
                             &QuadraticAssignmentProblem::fixedAssignments>)
            .def_property(
                    "forbidden_assignments",
                    [](const QuadraticAssignmentProblem& p) { return p.forbiddenAssignments; },
                    &setFact<std::vector<ForbiddenAssignment>,
                             &QuadraticAssignmentProblem::forbiddenAssignments>)
            // Start the next solve from the current assignment (a full bijection);
            // the solve resets it to False.
            .def_readwrite("warm_start", &QuadraticAssignmentProblem::warmStart)
            // No weight fields: weights come only from constraint selections; the
            // defaults are the components' kDefaultWeight (constraint_info()).
            // The score last computed on this problem (a copy): None until
            // calculate_score() or a solve has scored it.
            .def_property_readonly("score",
                                   [](const QuadraticAssignmentProblem& p) { return p.score; })
            .def("size", &QuadraticAssignmentProblem::size)
            .def("facility_count", &QuadraticAssignmentProblem::facilityCount)
            .def("location_count", &QuadraticAssignmentProblem::locationCount)
            .def("is_bijection", &QuadraticAssignmentProblem::isBijection)
            .def("flow_distance_cost",
                 [](QuadraticAssignmentProblem& p) { return prepared(p).flowDistanceCost(); })
            .def("assignment_cost",
                 [](QuadraticAssignmentProblem& p) { return prepared(p).assignmentCost(); })
            .def("total_cost",
                 [](QuadraticAssignmentProblem& p) { return prepared(p).totalCost(); })
            // Per location: the index of the facility standing there (-1 = empty).
            .def("assignment", &QuadraticAssignmentProblem::assignment)
            .def("set_assignment", &setAssignment, py::arg("assignment"));

    m.def(
            "calculate_score",
            [](QuadraticAssignmentProblem& problem) { return ScoreCalculator::score(problem); },
            py::arg("problem"),
            "Full recompute of the score at the default constraints (also "
            "stores problem.score).");

    py::enum_<optimaru::AcceptanceRule>(m, "AcceptorType", py::module_local())
            .value("HILL_CLIMBING", optimaru::AcceptanceRule::HillClimbing)
            .value("LATE_ACCEPTANCE", optimaru::AcceptanceRule::LateAcceptance)
            .value("ENTITY_TABU", optimaru::AcceptanceRule::EntityTabu)
            .value("SIMULATED_ANNEALING", optimaru::AcceptanceRule::SimulatedAnnealing)
            .value("GREAT_DELUGE", optimaru::AcceptanceRule::GreatDeluge)
            .value("STEP_COUNTING_HILL_CLIMBING",
                   optimaru::AcceptanceRule::StepCountingHillClimbing)
            .value("RECORD_TO_RECORD_TRAVEL", optimaru::AcceptanceRule::RecordToRecordTravel);

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            // The local-search acceptor (LATE_ACCEPTANCE by default).
            .def_readwrite("acceptor_type", &Solver::Config::rule)
            .def_property(
                    "entity_tabu_size", [](const Solver::Config& c) { return c.tabu_tenure; },
                    [](Solver::Config& c, int size) {
                        if (size < 0) throw py::value_error("entity_tabu_size must be >= 0");
                        c.tabu_tenure = size;
                    })
            // SIMULATED_ANNEALING: (0, 0) = auto, soft = the fraction below x
            // |the start's soft score| (the semiconductor-scheduling rule).
            .def_property(
                    "simulated_annealing_starting_temperature",
                    [](const Solver::Config& c) {
                        return py::make_tuple(c.start_temperature.hard, c.start_temperature.soft);
                    },
                    [](Solver::Config& c, std::pair<std::int64_t, double> hs) {
                        if (hs.first < 0 || !(hs.second >= 0.0) || !std::isfinite(hs.second))
                            throw py::value_error(
                                    "simulated_annealing_starting_temperature must be finite and "
                                    ">= 0");
                        c.start_temperature = Score{hs.first, hs.second};
                    })
            .def_property(
                    "simulated_annealing_temperature_fraction",
                    [](const Solver::Config& c) {
                        return c.simulated_annealing_temperature_fraction;
                    },
                    [](Solver::Config& c, double f) {
                        if (!(f > 0.0) || !std::isfinite(f))
                            throw py::value_error(
                                    "simulated_annealing_temperature_fraction must be finite and > "
                                    "0");
                        c.simulated_annealing_temperature_fraction = f;
                    });

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: starts from the identity (or the warm-start
            // bijection), searches with swaps, sets facility.location,
            // problem.score and warm_start = False. The GIL is released so
            // parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<QuadraticAssignmentProblem&>(&Solver::solve),
                 py::arg("problem"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.quadratic_assignment.constraints). The picks replace the defaults;
            // None keeps them. Single start. A name may be picked several times:
            // every pick is its own instance (a fresh copy of the default
            // component) with its own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, QuadraticAssignmentProblem& problem,
                       const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly. constraint_costs() reports the three side constraints per
    // location (each also lists the locations it evaluates as "units") and the two
    // objectives as one total each.
    opy::bind_constraints<QuadraticAssignmentProblem>(
            m, "problem", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
