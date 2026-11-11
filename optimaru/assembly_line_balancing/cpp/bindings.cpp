// pybind11 bindings for the assembly-line-balancing C++ domain.
// The Python extension links the alb::* implementations and imports the
// generic optimaru engine from optimaru/cpp/optimaru; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional conversions

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "alb/constraints.hpp"
#include "alb/domain.hpp"
#include "alb/score_calculator.hpp"
#include "alb/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"
#include "optimaru/python/names.hpp"

namespace py = pybind11;
using namespace alb;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Assembly-line balancing — optimaru engine-backed C++ wrapper "
            "exposed to Python";

    opy::bind_score<Score>(m);

    py::class_<Task>(m, "Task", py::module_local())
            .def(py::init([](std::string id, std::int64_t duration, int minStation,
                             std::optional<int> maxStation, std::optional<int> fixedStation,
                             std::vector<int> forbiddenStations, int station) {
                     Task task;
                     task.id = std::move(id);
                     task.duration = duration;
                     task.minStation = minStation;
                     task.maxStation = maxStation;
                     task.fixedStation = fixedStation;
                     task.forbiddenStations = std::move(forbiddenStations);
                     task.station = station;
                     return task;
                 }),
                 py::arg("id"), py::arg("duration"), py::arg("min_station") = 0,
                 py::arg("max_station") = py::none(), py::arg("fixed_station") = py::none(),
                 py::arg("forbidden_stations") = std::vector<int>{}, py::arg("station") = -1)
            .def_readwrite("id", &Task::id)
            .def_readwrite("duration", &Task::duration)
            .def_readwrite("min_station", &Task::minStation)
            .def_readwrite("max_station", &Task::maxStation)
            .def_readwrite("fixed_station", &Task::fixedStation)
            .def_readwrite("forbidden_stations", &Task::forbiddenStations)
            // The decision variable: a station index, -1 == unassigned.
            .def_readwrite("station", &Task::station)
            .def("is_assigned", &Task::isAssigned);

    py::class_<Precedence>(m, "Precedence", py::module_local())
            .def(py::init([](int before, int after) { return Precedence{before, after}; }),
                 py::arg("before"), py::arg("after"))
            .def_readwrite("before", &Precedence::before)
            .def_readwrite("after", &Precedence::after);

    py::class_<TaskGroup>(m, "TaskGroup", py::module_local())
            .def(py::init([](std::string id, std::vector<int> tasks) {
                     return TaskGroup{std::move(id), std::move(tasks)};
                 }),
                 py::arg("id"), py::arg("tasks"))
            .def_readwrite("id", &TaskGroup::id)
            .def_readwrite("tasks", &TaskGroup::tasks);

    py::class_<TaskPair>(m, "TaskPair", py::module_local())
            .def(py::init([](int first, int second) { return TaskPair{first, second}; }),
                 py::arg("first"), py::arg("second"))
            .def_readwrite("first", &TaskPair::first)
            .def_readwrite("second", &TaskPair::second);

    py::class_<AssemblyLineBalancingProblem>(m, "AssemblyLineBalancingProblem", py::module_local())
            .def(py::init([](std::int64_t cycleTime, std::vector<Task> tasks,
                             std::vector<Precedence> precedences, std::optional<int> maxStations,
                             std::vector<TaskGroup> sameStationGroups,
                             std::vector<TaskPair> differentStationPairs, bool warmStart) {
                     AssemblyLineBalancingProblem problem;
                     problem.cycleTime = cycleTime;
                     problem.tasks = std::move(tasks);
                     problem.precedences = std::move(precedences);
                     problem.maxStations = maxStations;
                     problem.sameStationGroups = std::move(sameStationGroups);
                     problem.differentStationPairs = std::move(differentStationPairs);
                     problem.warmStart = warmStart;
                     return problem;
                 }),
                 py::arg("cycle_time"), py::arg("tasks"),
                 py::arg("precedences") = std::vector<Precedence>{},
                 py::arg("max_stations") = py::none(),
                 py::arg("same_station_groups") = std::vector<TaskGroup>{},
                 py::arg("different_station_pairs") = std::vector<TaskPair>{},
                 py::arg("warm_start") = false)
            // NOTE: element access returns copies; use set_assignment for bulk
            // station writes.
            .def_readwrite("cycle_time", &AssemblyLineBalancingProblem::cycleTime)
            .def_readwrite("tasks", &AssemblyLineBalancingProblem::tasks)
            .def_readwrite("precedences", &AssemblyLineBalancingProblem::precedences)
            .def_readwrite("max_stations", &AssemblyLineBalancingProblem::maxStations)
            .def_readwrite("same_station_groups", &AssemblyLineBalancingProblem::sameStationGroups)
            .def_readwrite("different_station_pairs",
                           &AssemblyLineBalancingProblem::differentStationPairs)
            // Start the next solve from the current stations (tasks at -1 go
            // through the construction); the solve resets it to False.
            .def_readwrite("warm_start", &AssemblyLineBalancingProblem::warmStart)
            // No weight fields: weights come only from constraint selections; the
            // defaults are the components' kDefaultWeight (constraint_info()).
            .def_readonly("score", &AssemblyLineBalancingProblem::score)
            // The frozen value range of the last solve / scoring:
            // max(task count, 1 + max station).
            .def_readonly("station_count", &AssemblyLineBalancingProblem::stationCount)
            .def("task_count", &AssemblyLineBalancingProblem::taskCount)
            .def("station_loads", &AssemblyLineBalancingProblem::stationLoads)
            .def("stations_used", &AssemblyLineBalancingProblem::stationsUsed)
            .def("assignment", &AssemblyLineBalancingProblem::assignment)
            .def(
                    "set_assignment",
                    [](AssemblyLineBalancingProblem& problem, const std::vector<int>& stations) {
                        if (stations.size() != problem.tasks.size())
                            throw py::value_error("assignment length must equal the task count");
                        for (std::size_t i = 0; i < stations.size(); ++i) {
                            if (stations[i] < -1) {
                                throw py::value_error("task '" + problem.tasks[i].id +
                                                      "': station " + std::to_string(stations[i]) +
                                                      " is below -1");
                            }
                        }
                        for (std::size_t i = 0; i < stations.size(); ++i)
                            problem.tasks[i].station = stations[i];
                    },
                    py::arg("assignment"));

    m.def(
            "calculate_score",
            [](AssemblyLineBalancingProblem& problem) {
                ScoreCalculator::prepare(problem);
                return ScoreCalculator::score(problem);
            },
            py::arg("problem"),
            "Full recompute of the score at the default constraints (also "
            "stores problem.score).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Guided change-move targets from each task's precedence / domain
            // window. probability 0 = off.
            .def_readwrite("value_recommender_probability",
                           &Solver::Config::value_recommender_probability)
            // Assignment best-improvement descent strategy weight. 0 = off.
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            // Gate on how often a step may draw CloseStationMove. 1 = engine
            // default (unrestricted).
            .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
            // CloseStationMove: the domain macro move that empties one station in
            // a single step (see cpp/src/solver.cpp).
            .def_readwrite("close_station_moves", &Solver::Config::close_station_moves);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: constructs, searches, sets task.station,
            // problem.score and warm_start = False. The GIL is released so
            // parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<AssemblyLineBalancingProblem&>(&Solver::solve),
                 py::arg("problem"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.assembly_line_balancing.constraints). The picks replace the
            // defaults; None keeps them. Single start. A name may be picked
            // several times: every pick is its own instance (a fresh copy of the
            // default component) with its own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, AssemblyLineBalancingProblem& problem,
                       const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `problem` — the input
    // optimaru.assembly_line_balancing.constraints.score() turns into a score — in
    // catalog (list_constraints()) order. Scoring once is enough: re-pricing
    // any assembly is pure Python arithmetic on these. None = the default
    // assembly; a pick list scores exactly those picks (apply_constraints),
    // each entry carrying its pick's knobs. The keys are opy::constraint_costs'
    // (the k-th instance of a name "<name>#<k>", in calculator order); only
    // the order of the names follows the catalog, each name's instances stay
    // together in their own order.
    m.def(
            "constraint_costs",
            [](AssemblyLineBalancingProblem& problem, const py::object& constraints) {
                ScoreCalculator::prepare(problem);
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                const py::dict costs = opy::constraint_costs(calculator, problem);
                py::dict ordered;
                for (const std::string& name : catalogNames()) {
                    const std::string python_name = opy::python_name(name);
                    for (const auto& item : costs) {
                        if (py::reinterpret_borrow<py::dict>(item.second)["name"]
                                    .cast<std::string>() == python_name)
                            ordered[item.first] = item.second;
                    }
                }
                return ordered;
            },
            py::arg("problem"), py::arg("constraints") = py::none(),
            "Raw per-unit cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in catalog order — the same
    // strings a score breakdown reports.
    m.def(
            "list_constraints",
            [] {
                std::vector<std::string> names;
                for (const std::string& name : catalogNames())
                    names.push_back(opy::python_name(name));
                return names;
            },
            "Constraint names available to optimaru.assembly_line_balancing.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    // (The rows come from catalogRows(), the assembly's info() in catalog
    // order: an assembly lists its unit components before its globals, and
    // this domain interleaves them.)
    opy::detail::bind_info_rows(m, [] { return catalogRows(); });
}
