// pybind11 bindings for the max-cut C++ domain.
// The Python extension links the mc::* implementations and imports the
// generic optimaru engine from ../../../include; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::optional conversions

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mc/constraints.hpp"
#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"
#include "mc/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace mc;
namespace tfpy = optimaru::python;

namespace
{

// One side per vertex, each -1 / 0 / 1 (`what` names the field in errors).
void requireSides(const MaxCutProblem &problem, const std::vector<int> &sides, const char *what)
{
    if (sides.size() != problem.vertexIds.size())
    {
        throw py::value_error(std::string(what) + " length must equal the vertex count ("
                              + std::to_string(problem.vertexIds.size()) + "), not "
                              + std::to_string(sides.size()));
    }
    for (std::size_t v = 0; v < sides.size(); ++v)
    {
        if (sides[v] < kUnassigned || sides[v] >= kSideCount)
        {
            throw py::value_error("vertex '" + problem.vertexIds[v] + "': " + what + " must be -1, 0 or 1, not "
                                  + std::to_string(sides[v]));
        }
    }
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Max-Cut — optimaru engine-backed C++ wrapper exposed to Python";

    // Every class is module_local: the server's isolated runtime loads several
    // domains whose classes share names (Score, Settings, Solver).
    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__", [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Edge>(m, "Edge", py::module_local())
        .def(py::init([](int source, int target, std::int64_t weight) { return Edge{source, target, weight}; }),
             py::arg("source"),
             py::arg("target"),
             py::arg("weight") = 1)
        // Vertex indices (positions in vertex_ids). Undirected; a self-loop is
        // never cut, parallel edges add up.
        .def_readwrite("source", &Edge::source)
        .def_readwrite("target", &Edge::target)
        .def_readwrite("weight", &Edge::weight);

    py::class_<VertexPair>(m, "VertexPair", py::module_local())
        .def(py::init([](int first, int second) { return VertexPair{first, second}; }),
             py::arg("first"),
             py::arg("second"))
        .def_readwrite("first", &VertexPair::first)
        .def_readwrite("second", &VertexPair::second);

    py::class_<MaxCutProblem>(m, "MaxCutProblem", py::module_local())
        .def(py::init(
                 [](std::vector<std::string> vertexIds,
                    std::vector<Edge> edges,
                    std::optional<std::vector<int>> fixedSide,
                    std::vector<VertexPair> samePairs,
                    std::vector<VertexPair> differentPairs,
                    std::optional<int> minSize,
                    std::optional<int> max_size,
                    std::optional<std::vector<int>> side,
                    bool warmStart)
                 {
                     MaxCutProblem problem;
                     problem.vertexIds = std::move(vertexIds);
                     problem.edges = std::move(edges);
                     problem.fixedSide = fixedSide ? std::move(*fixedSide)
                                                   : std::vector<int>(problem.vertexIds.size(), kUnassigned);
                     requireSides(problem, problem.fixedSide, "fixed_side");
                     problem.samePartitionPairs = std::move(samePairs);
                     problem.differentPartitionPairs = std::move(differentPairs);
                     problem.minPartitionSize = minSize;
                     problem.maxPartitionSize = max_size;
                     problem.side = side ? std::move(*side) : std::vector<int>(problem.vertexIds.size(), kUnassigned);
                     requireSides(problem, problem.side, "side");
                     problem.warmStart = warmStart;
                     return problem;
                 }),
             py::arg("vertex_ids"),
             py::arg("edges") = std::vector<Edge>{},
             py::arg("fixed_side") = py::none(),
             py::arg("same_partition_pairs") = std::vector<VertexPair>{},
             py::arg("different_partition_pairs") = std::vector<VertexPair>{},
             py::arg("min_partition_size") = py::none(),
             py::arg("max_partition_size") = py::none(),
             py::arg("side") = py::none(),
             py::arg("warm_start") = false)
        // NOTE: list access returns copies; assign whole lists (and use
        // set_assignment for the partition). Every entry point (solve,
        // calculate_score, constraint_costs) re-validates and re-indexes the
        // facts, so edits between calls are always picked up.
        .def_readwrite("vertex_ids", &MaxCutProblem::vertexIds)
        .def_readwrite("edges", &MaxCutProblem::edges)
        // Per vertex: -1 = free, 0 / 1 = pinned to that side.
        .def_readwrite("fixed_side", &MaxCutProblem::fixedSide)
        .def_readwrite("same_partition_pairs", &MaxCutProblem::samePartitionPairs)
        .def_readwrite("different_partition_pairs", &MaxCutProblem::differentPartitionPairs)
        // None = 0 / the vertex count.
        .def_readwrite("min_partition_size", &MaxCutProblem::minPartitionSize)
        .def_readwrite("max_partition_size", &MaxCutProblem::maxPartitionSize)
        // Start the next solve from the current partition (unassigned
        // vertices take the default start); the solve resets it to False.
        .def_readwrite("warm_start", &MaxCutProblem::warmStart)
        // No weight fields: weights come only from constraint selections; the
        // defaults are the components' kDefaultWeight (constraint_info()).
        .def_readonly("score", &MaxCutProblem::score)
        // True once calculate_score or a solve has scored the partition.
        .def_readonly("scored", &MaxCutProblem::scored)
        .def("vertex_count", &MaxCutProblem::vertexCount)
        .def("cut_weight", &MaxCutProblem::cutWeight)
        .def("total_edge_weight", &MaxCutProblem::totalEdgeWeight)
        .def(
            "side_size",
            [](const MaxCutProblem &problem, int side)
            {
                if (side < 0 || side >= kSideCount)
                    throw py::value_error("side must be 0 or 1, not " + std::to_string(side));
                return problem.sideSize(side);
            },
            py::arg("side"))
        .def("is_exact_size", &MaxCutProblem::exactSize)
        // Every vertex on side 0 or 1 (no -1).
        .def("is_complete", &MaxCutProblem::isComplete)
        // The planning variable: the side per vertex, -1 = unassigned.
        .def("assignment", [](const MaxCutProblem &problem) { return problem.side; })
        .def(
            "set_assignment",
            [](MaxCutProblem &problem, const std::vector<int> &sides)
            {
                requireSides(problem, sides, "side");
                problem.side = sides;
            },
            py::arg("assignment"));

    m.def(
        "calculate_score",
        [](MaxCutProblem &problem) { return ScoreCalculator::score(problem); },
        py::arg("problem"),
        "Full recompute of the HardSoftScore at the default constraints (also "
        "stores problem.score and sets problem.scored). Unassigned vertices "
        "(-1) follow the C++ rule: in no side.");

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
        // 0 = auto: clamp(8 * vertices, 400, 3200).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.history_length; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error("late_acceptance_size must be >= 0 (0 = auto)");
                c.history_length = static_cast<std::size_t>(size);
            })
        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c) { return c.score_checks == optimaru::ScoreChecks::Full; },
            [](Solver::Config &c, bool on)
            {
                c.score_checks
                    = on ? optimaru::ScoreChecks::Full : optimaru::ScoreChecks::Off;
            })
        // Ruin-and-recreate (kicks and the as-move) is off in exact-size mode.
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
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
        // ruin_recreate.as_move_weight). 0 (default) disables it.
        .def_readwrite("ruin_recreate_as_move_weight", &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin", &Solver::Config::ruinRecreateAsMoveMaxRuin)
        // A change move flips one vertex; a swap move exchanges a side-0 and a
        // side-1 vertex. Exact-size mode forces 0 / 1 (swaps only).
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        // BASIC best-improvement descent strategy weight. 0 = off.
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
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
        // Solves in place: builds the start partition, searches, sets the
        // partition, problem.score, scored = True and warm_start = False. The
        // GIL is released so parallel multi-start workers can fire
        // on_new_best.
        .def("solve",
             py::overload_cast<MaxCutProblem &>(&Solver::solve),
             py::arg("problem"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // max_cut.constraints). The picks replace the defaults; None keeps
        // them. Single start. A name may be picked several times: every pick
        // is its own instance (a fresh copy of the default component) with
        // its own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver, MaxCutProblem &problem, const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
                py::gil_scoped_release release;
                solver.solve(problem, calculator);
            },
            py::arg("problem"),
            py::arg("constraints") = py::none());

    // The raw costs of an assembly on `problem` — the input
    // max_cut.constraints.score() turns into a score: the four unit
    // constraints per side ([side 0, side 1]), the two globals as one total.
    // None = the default assembly in list_constraints() order; a pick list
    // scores exactly those picks (apply_constraints), each entry carrying its
    // pick's knobs, the k-th pick of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](MaxCutProblem &problem, const py::object &constraints)
        {
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            return tfpy::constraint_costs(calculator, problem);
        },
        py::arg("problem"),
        py::arg("constraints") = py::none(),
        "Raw cost of every constraint (per side, or one total for the globals), before "
        "weight/exponent/offset/level.");

    // The constraint names this domain scores, in catalog order — the same
    // strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to max_cut.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
