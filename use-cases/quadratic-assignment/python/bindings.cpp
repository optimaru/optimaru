// pybind11 bindings for the quadratic-assignment C++ domain.
// The Python extension links the qap::* implementations and imports the
// generic optimaru engine from ../../../include; the engine itself is not
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
#include "qap/constraints.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"
#include "qap/solver.hpp"

namespace py = pybind11;
using namespace qap;
namespace tfpy = optimaru::python;

namespace
{

using Matrix = std::vector<std::vector<std::int64_t>>;

// A fact setter: write the value and drop the prepared caches, so the next
// score / solve re-validates and rebuilds them.
template <typename T, T QuadraticAssignmentProblem::*Member>
void setFact(QuadraticAssignmentProblem &problem, T value)
{
    problem.*Member = std::move(value);
    problem.prepared = false;
}

// Validated facts, then a read (the totals index the matrices directly).
QuadraticAssignmentProblem &prepared(QuadraticAssignmentProblem &problem)
{
    ScoreCalculator::prepare(problem);
    return problem;
}

void setAssignment(QuadraticAssignmentProblem &problem, const std::vector<int> &assignment)
{
    if (assignment.size() != problem.locations.size())
        throw py::value_error("assignment length must equal the location count");
    std::vector<int> locationOf(problem.facilities.size(), -1);
    for (std::size_t l = 0; l < assignment.size(); ++l)
    {
        const int f = assignment[l];
        if (f == -1)
            continue;
        if (f < -1 || f >= problem.facilityCount())
        {
            throw py::value_error("location '" + problem.locations[l].id + "': facility index " + std::to_string(f)
                                  + " out of range (-1 = empty)");
        }
        if (locationOf[f] >= 0)
        {
            throw py::value_error("facility '" + problem.facilities[f].id + "' is assigned to both '"
                                  + problem.locations[locationOf[f]].id + "' and '" + problem.locations[l].id + "'");
        }
        locationOf[f] = static_cast<int>(l);
    }
    for (std::size_t f = 0; f < problem.facilities.size(); ++f)
        problem.facilities[f].location = locationOf[f];
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Quadratic assignment — optimaru engine-backed C++ wrapper exposed to Python";

    // Every class is module_local: the server's isolated runtime loads several
    // domains whose classes share names (Score, Settings, Solver).
    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__", [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Facility>(m, "Facility", py::module_local())
        .def(py::init(
                 [](std::string id, std::optional<std::int64_t> demand, int location)
                 { return Facility{std::move(id), demand, location}; }),
             py::arg("id"),
             py::arg("demand") = py::none(),
             py::arg("location") = -1)
        .def_readwrite("id", &Facility::id)
        // facility_demands[id]; None = not listed (scored as demand 0).
        .def_readwrite("demand", &Facility::demand)
        // The planning variable: index into problem.locations; -1 == unassigned.
        .def_readwrite("location", &Facility::location)
        .def("is_assigned", &Facility::isAssigned);

    py::class_<Location>(m, "Location", py::module_local())
        .def(py::init([](std::string id, std::optional<std::int64_t> capacity)
                      { return Location{std::move(id), capacity}; }),
             py::arg("id"),
             py::arg("capacity") = py::none())
        .def_readwrite("id", &Location::id)
        // location_capacities[id]; None = no capacity limit.
        .def_readwrite("capacity", &Location::capacity);

    py::class_<FixedAssignment>(m, "FixedAssignment", py::module_local())
        .def(py::init([](int location, int facility) { return FixedAssignment{location, facility}; }),
             py::arg("location"),
             py::arg("facility"))
        .def_readwrite("location", &FixedAssignment::location)
        .def_readwrite("facility", &FixedAssignment::facility);

    py::class_<ForbiddenAssignment>(m, "ForbiddenAssignment", py::module_local())
        .def(py::init([](int facility, int location) { return ForbiddenAssignment{facility, location}; }),
             py::arg("facility"),
             py::arg("location"))
        .def_readwrite("facility", &ForbiddenAssignment::facility)
        .def_readwrite("location", &ForbiddenAssignment::location);

    py::class_<QuadraticAssignmentProblem>(m, "QuadraticAssignmentProblem", py::module_local())
        .def(py::init(
                 [](std::vector<Facility> facilities,
                    std::vector<Location> locations,
                    Matrix flows,
                    Matrix distances,
                    std::optional<Matrix> assignmentCosts,
                    std::vector<FixedAssignment> fixedAssignments,
                    std::vector<ForbiddenAssignment> forbiddenAssignments,
                    bool warmStart)
                 {
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
             py::arg("facilities"),
             py::arg("locations"),
             py::arg("flows"),
             py::arg("distances"),
             py::arg("assignment_costs") = py::none(),
             py::arg("fixed_assignments") = std::vector<FixedAssignment>{},
             py::arg("forbidden_assignments") = std::vector<ForbiddenAssignment>{},
             py::arg("warm_start") = false)
        // NOTE: element access returns copies; use set_assignment for bulk
        // location writes. Assigning a fact drops the prepared caches.
        .def_property("facilities",
                      [](const QuadraticAssignmentProblem &p) { return p.facilities; },
                      &setFact<std::vector<Facility>, &QuadraticAssignmentProblem::facilities>)
        .def_property("locations",
                      [](const QuadraticAssignmentProblem &p) { return p.locations; },
                      &setFact<std::vector<Location>, &QuadraticAssignmentProblem::locations>)
        .def_property("flows",
                      [](const QuadraticAssignmentProblem &p) { return p.flows; },
                      &setFact<Matrix, &QuadraticAssignmentProblem::flows>)
        .def_property("distances",
                      [](const QuadraticAssignmentProblem &p) { return p.distances; },
                      &setFact<Matrix, &QuadraticAssignmentProblem::distances>)
        .def_property("assignment_costs",
                      [](const QuadraticAssignmentProblem &p) { return p.assignmentCosts; },
                      &setFact<std::optional<Matrix>, &QuadraticAssignmentProblem::assignmentCosts>)
        .def_property("fixed_assignments",
                      [](const QuadraticAssignmentProblem &p) { return p.fixedAssignments; },
                      &setFact<std::vector<FixedAssignment>, &QuadraticAssignmentProblem::fixedAssignments>)
        .def_property("forbidden_assignments",
                      [](const QuadraticAssignmentProblem &p) { return p.forbiddenAssignments; },
                      &setFact<std::vector<ForbiddenAssignment>, &QuadraticAssignmentProblem::forbiddenAssignments>)
        // Start the next solve from the current assignment (a full bijection);
        // the solve resets it to False.
        .def_readwrite("warm_start", &QuadraticAssignmentProblem::warmStart)
        // No weight fields: weights come only from constraint selections; the
        // defaults are the components' kDefaultWeight (constraint_info()).
        // The score last computed on this problem (a copy): None until
        // calculate_score() or a solve has scored it.
        .def_property_readonly("score", [](const QuadraticAssignmentProblem &p) { return p.score; })
        .def("size", &QuadraticAssignmentProblem::size)
        .def("facility_count", &QuadraticAssignmentProblem::facilityCount)
        .def("location_count", &QuadraticAssignmentProblem::locationCount)
        .def("is_bijection", &QuadraticAssignmentProblem::isBijection)
        .def("flow_distance_cost",
             [](QuadraticAssignmentProblem &p) { return prepared(p).flowDistanceCost(); })
        .def("assignment_cost", [](QuadraticAssignmentProblem &p) { return prepared(p).assignmentCost(); })
        .def("total_cost", [](QuadraticAssignmentProblem &p) { return prepared(p).totalCost(); })
        // Per location: the index of the facility standing there (-1 = empty).
        .def("assignment", &QuadraticAssignmentProblem::assignment)
        .def("set_assignment", &setAssignment, py::arg("assignment"));

    m.def(
        "calculate_score",
        [](QuadraticAssignmentProblem &problem) { return ScoreCalculator::score(problem); },
        py::arg("problem"),
        "Full recompute of the score at the default constraints (also "
        "stores problem.score).");

    py::enum_<optimaru::AcceptanceRule>(m, "AcceptorType", py::module_local())
        .value("HILL_CLIMBING", optimaru::AcceptanceRule::HillClimbing)
        .value("LATE_ACCEPTANCE", optimaru::AcceptanceRule::LateAcceptance)
        .value("ENTITY_TABU", optimaru::AcceptanceRule::EntityTabu)
        .value("SIMULATED_ANNEALING", optimaru::AcceptanceRule::SimulatedAnnealing)
        .value("GREAT_DELUGE", optimaru::AcceptanceRule::GreatDeluge)
        .value("STEP_COUNTING_HILL_CLIMBING", optimaru::AcceptanceRule::StepCountingHillClimbing)
        .value("RECORD_TO_RECORD_TRAVEL", optimaru::AcceptanceRule::RecordToRecordTravel);

    optimaru::python::bind_config_copy(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c) { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.time_limit = optimaru::python::seconds_to_millis(s, "spent_limit_seconds"); })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c) { return std::chrono::duration<double>(c.stall_time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "unimproved_spent_limit_seconds"); })
        // 0 = no step limit.
        .def_readwrite("step_count_limit", &Solver::Config::step_limit)
        .def_readwrite("unimproved_step_count_limit", &Solver::Config::stall_step_limit)
        .def_readwrite("random_seed", &Solver::Config::seed)
        .def_property(
            "parallel_solver_count",
            [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error("parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // The local-search acceptor (LATE_ACCEPTANCE by default).
        .def_readwrite("acceptor_type", &Solver::Config::rule)
        // 0 = auto: clamp(8 * facilities, 400, 3200).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.history_length; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error("late_acceptance_size must be >= 0 (0 = auto)");
                c.history_length = static_cast<std::size_t>(size);
            })
        .def_property(
            "entity_tabu_size",
            [](const Solver::Config &c) { return c.tabu_tenure; },
            [](Solver::Config &c, int size)
            {
                if (size < 0)
                    throw py::value_error("entity_tabu_size must be >= 0");
                c.tabu_tenure = size;
            })
        // SIMULATED_ANNEALING: (0, 0) = auto, soft = the fraction below x
        // |the start's soft score| (the semiconductor-scheduling rule).
        .def_property(
            "simulated_annealing_starting_temperature",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.start_temperature.hard,
                                      c.start_temperature.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            {
                if (hs.first < 0 || !(hs.second >= 0.0) || !std::isfinite(hs.second))
                    throw py::value_error("simulated_annealing_starting_temperature must be finite and >= 0");
                c.start_temperature = Score{hs.first, hs.second};
            })
        .def_property(
            "simulated_annealing_temperature_fraction",
            [](const Solver::Config &c) { return c.simulatedAnnealingTemperatureFraction; },
            [](Solver::Config &c, double f)
            {
                if (!(f > 0.0) || !std::isfinite(f))
                    throw py::value_error("simulated_annealing_temperature_fraction must be finite and > 0");
                c.simulatedAnnealingTemperatureFraction = f;
            })
        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c) { return c.score_checks == optimaru::ScoreChecks::Full; },
            [](Solver::Config &c, bool on)
            { c.score_checks = on ? optimaru::ScoreChecks::Full : optimaru::ScoreChecks::Off; })
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c) { return py::make_tuple(c.target_score.hard, c.target_score.soft); },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs) { c.target_score = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: starts from the identity (or the warm-start
        // bijection), searches with swaps, sets facility.location,
        // problem.score and warm_start = False. The GIL is released so
        // parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<QuadraticAssignmentProblem &>(&Solver::solve),
             py::arg("problem"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // quadratic_assignment.constraints). The picks replace the defaults;
        // None keeps them. Single start. A name may be picked several times:
        // every pick is its own instance (a fresh copy of the default
        // component) with its own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver, QuadraticAssignmentProblem &problem, const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
                py::gil_scoped_release release;
                solver.solve(problem, calculator);
            },
            py::arg("problem"),
            py::arg("constraints") = py::none());

    // The raw costs of an assembly on `problem` — the input
    // quadratic_assignment.constraints.score() turns into a score: the three
    // side constraints per location (in location order; each also reports the
    // locations it evaluates as "units"), the two objectives as one total
    // each. None = the default assembly in list_constraints() order; a pick
    // list scores exactly those picks (apply_constraints), each entry carrying
    // its pick's knobs, the k-th pick of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](QuadraticAssignmentProblem &problem, const py::object &constraints)
        {
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            return tfpy::constraint_costs(calculator, problem);
        },
        py::arg("problem"),
        py::arg("constraints") = py::none(),
        "Raw cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in catalog order — the same
    // strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to quadratic_assignment.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
