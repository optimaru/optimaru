// pybind11 bindings for the assembly-line-balancing C++ domain.
// The Python extension links the alb::* implementations and imports the
// generic optimaru engine from ../../../include; the engine itself is not
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
#include "optimaru/python/names.hpp"

namespace py = pybind11;
using namespace alb;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Assembly-line balancing — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__", [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Task>(m, "Task", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::int64_t duration,
                    int minStation,
                    std::optional<int> maxStation,
                    std::optional<int> fixedStation,
                    std::vector<int> forbiddenStations,
                    int station)
                 {
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
             py::arg("id"),
             py::arg("duration"),
             py::arg("min_station") = 0,
             py::arg("max_station") = py::none(),
             py::arg("fixed_station") = py::none(),
             py::arg("forbidden_stations") = std::vector<int>{},
             py::arg("station") = -1)
        .def_readwrite("id", &Task::id)
        .def_readwrite("duration", &Task::duration)
        .def_readwrite("min_station", &Task::minStation)
        .def_readwrite("max_station", &Task::maxStation)
        .def_readwrite("fixed_station", &Task::fixedStation)
        .def_readwrite("forbidden_stations", &Task::forbiddenStations)
        // The planning variable: a station index, -1 == unassigned.
        .def_readwrite("station", &Task::station)
        .def("is_assigned", &Task::isAssigned);

    py::class_<Precedence>(m, "Precedence", py::module_local())
        .def(py::init([](int before, int after) { return Precedence{before, after}; }),
             py::arg("before"),
             py::arg("after"))
        .def_readwrite("before", &Precedence::before)
        .def_readwrite("after", &Precedence::after);

    py::class_<TaskGroup>(m, "TaskGroup", py::module_local())
        .def(py::init([](std::string id, std::vector<int> tasks)
                      { return TaskGroup{std::move(id), std::move(tasks)}; }),
             py::arg("id"),
             py::arg("tasks"))
        .def_readwrite("id", &TaskGroup::id)
        .def_readwrite("tasks", &TaskGroup::tasks);

    py::class_<TaskPair>(m, "TaskPair", py::module_local())
        .def(py::init([](int first, int second) { return TaskPair{first, second}; }),
             py::arg("first"),
             py::arg("second"))
        .def_readwrite("first", &TaskPair::first)
        .def_readwrite("second", &TaskPair::second);

    py::class_<AssemblyLineBalancingProblem>(m, "AssemblyLineBalancingProblem", py::module_local())
        .def(py::init(
                 [](std::int64_t cycleTime,
                    std::vector<Task> tasks,
                    std::vector<Precedence> precedences,
                    std::optional<int> maxStations,
                    std::vector<TaskGroup> sameStationGroups,
                    std::vector<TaskPair> differentStationPairs,
                    bool warmStart)
                 {
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
             py::arg("cycle_time"),
             py::arg("tasks"),
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
        .def_readwrite("different_station_pairs", &AssemblyLineBalancingProblem::differentStationPairs)
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
            [](AssemblyLineBalancingProblem &problem, const std::vector<int> &stations)
            {
                if (stations.size() != problem.tasks.size())
                    throw py::value_error("assignment length must equal the task count");
                for (std::size_t i = 0; i < stations.size(); ++i)
                {
                    if (stations[i] < -1)
                    {
                        throw py::value_error("task '" + problem.tasks[i].id + "': station "
                                              + std::to_string(stations[i]) + " is below -1");
                    }
                }
                for (std::size_t i = 0; i < stations.size(); ++i)
                    problem.tasks[i].station = stations[i];
            },
            py::arg("assignment"));

    m.def(
        "calculate_score",
        [](AssemblyLineBalancingProblem &problem)
        {
            ScoreCalculator::prepare(problem);
            return ScoreCalculator::calculateScore(problem);
        },
        py::arg("problem"),
        "Full recompute of the HardSoftScore at the default constraints (also "
        "stores problem.score).");

    optimaru::python::bindConfigCopy(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c) { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Solver::Config &c, double s)
            { c.spentLimit = optimaru::python::secondsToMillis(s, "spent_limit_seconds"); })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c) { return std::chrono::duration<double>(c.unimprovedSpentLimit).count(); },
            [](Solver::Config &c, double s)
            { c.unimprovedSpentLimit = optimaru::python::secondsToMillis(s, "unimproved_spent_limit_seconds"); })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit", &Solver::Config::unimprovedStepCountLimit)
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
        .def_property(
            "parallel_solver_count",
            [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error("parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // 0 = auto: clamp(8 * tasks, 400, 3200).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.lateAcceptanceSize; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error("late_acceptance_size must be >= 0 (0 = auto)");
                c.lateAcceptanceSize = static_cast<std::size_t>(size);
            })
        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c) { return c.environmentMode == optimaru::EnvironmentMode::FULL_ASSERT; },
            [](Solver::Config &c, bool on)
            {
                c.environmentMode
                    = on ? optimaru::EnvironmentMode::FULL_ASSERT : optimaru::EnvironmentMode::NON_ASSERT;
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruinRecreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger", &Solver::Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction", &Solver::Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction", &Solver::Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count", &Solver::Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_operator_decay", &Solver::Config::ruinRecreateOperatorDecay)
        .def_property(
            "ruin_recreate_rewards",
            [](const Solver::Config &c) { return c.ruinRecreateRewards; },
            [](Solver::Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error("ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps", &Solver::Config::ruinRecreatePolishSteps)
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruinRecreate.asMoveWeight). 0 disables it.
        .def_readwrite("ruin_recreate_as_move_weight", &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin", &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Guided change-move targets from each task's precedence / domain
        // window. probability 0 = off.
        .def_readwrite("value_recommender_probability", &Solver::Config::valueRecommenderProbability)
        // BASIC best-improvement descent strategy weight. 0 = off.
        .def_readwrite("descent_weight", &Solver::Config::descentWeight)
        // Gate on how often a step may draw CloseStationMove. 1 = engine
        // default (unrestricted).
        .def_readwrite("custom_move_probability", &Solver::Config::customMoveProbability)
        // CloseStationMove: the domain macro move that empties one station in
        // a single step (see cpp/src/solver.cpp).
        .def_readwrite("close_station_moves", &Solver::Config::closeStationMoves)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c) { return py::make_tuple(c.bestScoreLimit.hard, c.bestScoreLimit.soft); },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs) { c.bestScoreLimit = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: constructs, searches, sets task.station,
        // problem.score and warm_start = False. The GIL is released so
        // parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<AssemblyLineBalancingProblem &>(&Solver::solve),
             py::arg("problem"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // assembly_line_balancing.constraints). The picks replace the
        // defaults; None keeps them. Single start.
        .def(
            "solve_with_constraints",
            [](Solver &solver, AssemblyLineBalancingProblem &problem, const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints).into(calculator);
                py::gil_scoped_release release;
                solver.solve(problem, calculator);
            },
            py::arg("problem"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `problem` — the input
    // assembly_line_balancing.constraints.score() turns into a score — keyed in
    // catalog (list_constraints()) order. Scoring once is enough: re-pricing
    // any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](AssemblyLineBalancingProblem &problem)
        {
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            const py::dict costs = tfpy::constraintCosts(calculator, problem);
            py::dict ordered;
            for (const std::string &name : catalogNames())
            {
                const std::string key = tfpy::pythonName(name);
                ordered[py::str(key)] = costs[py::str(key)];
            }
            return ordered;
        },
        py::arg("problem"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in catalog order — the same
    // strings a score breakdown reports.
    m.def(
        "list_constraints",
        []
        {
            std::vector<std::string> names;
            for (const std::string &name : catalogNames())
                names.push_back(tfpy::pythonName(name));
            return names;
        },
        "Constraint names available to assembly_line_balancing.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    // (The rows come from catalogRows(), the assembly's info() in catalog
    // order: an assembly lists its unit components before its globals, and
    // this domain interleaves them.)
    tfpy::detail::bindInfoRows(m, [] { return catalogRows(); });
}
