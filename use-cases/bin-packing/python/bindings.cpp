// pybind11 bindings for the bin-packing C++ domain.
// The Python extension links the bp::* implementations and imports the
// generic optimaru engine from ../../../include; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional conversions

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bp/constraints.hpp"
#include "bp/domain.hpp"
#include "bp/score_calculator.hpp"
#include "bp/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace bp;
namespace tfpy = optimaru::python;

namespace
{

std::string itemRepr(const Item &item)
{
    std::string color = item.color ? py::repr(py::str(*item.color)).cast<std::string>() : "None";
    return "Item(id=" + py::repr(py::str(item.id)).cast<std::string>()
           + ", weight=" + std::to_string(item.weight) + ", bin=" + std::to_string(item.bin)
           + ", color=" + color + ")";
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Bin packing — optimaru engine-backed C++ wrapper exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def(py::init(
                 [](std::int64_t hard, double soft)
                 {
                     Score score;
                     score.hard = hard;
                     score.soft = soft;
                     return score;
                 }),
             py::arg("hard") = 0,
             py::arg("soft") = 0.0)
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        // Lexicographic (hard first), the engine's own ordering.
        .def(py::self == py::self)
        .def("__ne__", [](const Score &a, const Score &b) { return !(a == b); })
        .def(py::self < py::self)
        .def(py::self <= py::self)
        .def(py::self > py::self)
        .def(py::self >= py::self)
        .def("__hash__",
             [](const Score &s) { return py::hash(py::make_tuple(s.hard, s.soft)); })
        .def("__str__", &Score::str)
        .def("__repr__", [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Item>(m, "Item", py::module_local())
        .def(py::init(
                 [](std::string id, std::int64_t weight, int bin, std::optional<std::string> color)
                 {
                     Item item;
                     item.id = std::move(id);
                     item.weight = weight;
                     item.bin = bin;
                     item.color = std::move(color);
                     return item;
                 }),
             py::arg("id"),
             py::arg("weight"),
             py::arg("bin") = -1,
             py::arg("color") = py::none())
        .def_readwrite("id", &Item::id)
        .def_readwrite("weight", &Item::weight)
        // The planning variable: a 0-based bin index, -1 == unassigned.
        .def_readwrite("bin", &Item::bin)
        .def_readwrite("color", &Item::color)
        .def("is_assigned", &Item::isAssigned)
        .def("__repr__", &itemRepr);

    py::class_<BinPackingProblem>(m, "BinPackingProblem", py::module_local())
        .def(py::init(
                 [](std::int64_t binCapacity,
                    std::vector<Item> items,
                    std::vector<std::pair<std::string, std::string>> conflicts,
                    bool warmStart)
                 {
                     BinPackingProblem problem;
                     problem.binCapacity = binCapacity;
                     problem.items = std::move(items);
                     problem.conflicts = std::move(conflicts);
                     problem.warmStart = warmStart;
                     return problem;
                 }),
             py::arg("bin_capacity"),
             py::arg("items"),
             py::arg("conflicts") = std::vector<std::pair<std::string, std::string>>{},
             py::arg("warm_start") = false)
        .def_readwrite("bin_capacity", &BinPackingProblem::binCapacity)
        // NOTE: element access returns copies (item.bin = x on
        // problem.items[i] is lost); use set_assignment for bin writes.
        // Replacing the list (or `conflicts`) rebuilds the derived lookups at
        // the next scoring / solving call.
        .def_property(
            "items",
            [](const BinPackingProblem &problem) { return problem.items; },
            [](BinPackingProblem &problem, std::vector<Item> items)
            {
                problem.items = std::move(items);
                problem.prepared = false;
            })
        .def_property(
            "conflicts",
            [](const BinPackingProblem &problem) { return problem.conflicts; },
            [](BinPackingProblem &problem, std::vector<std::pair<std::string, std::string>> conflicts)
            {
                problem.conflicts = std::move(conflicts);
                problem.prepared = false;
            })
        // true = the next solve starts from the items' bins (a seed); every
        // solve clears it.
        .def_readwrite("warm_start", &BinPackingProblem::warmStart)
        // No weight fields (removed in 0.2.0): weights come only from
        // constraint selections; the defaults are the components'
        // kDefaultWeight, reported by constraint_info().
        .def_readonly("score", &BinPackingProblem::score)
        .def("item_count", &BinPackingProblem::itemCount)
        .def("bins_used", &BinPackingProblem::binsUsed)
        .def("bin_loads", &BinPackingProblem::binLoads)
        // Convenience: the current bin per item (the solution).
        .def("assignment", &BinPackingProblem::assignment)
        // Bulk write of the planning variable (item access via .items
        // returns copies, so per-item writes would be lost).
        .def(
            "set_assignment",
            [](BinPackingProblem &problem, const std::vector<int> &bins)
            {
                if (bins.size() != problem.items.size())
                    throw py::value_error("assignment length must equal the item count");
                problem.setAssignment(bins);
            },
            py::arg("assignment"));

    // Full recompute of the Score (also stores problem.score).
    // constraints=None scores the default set (the monolithic calculator,
    // which knows the default picks only); a list scores exactly those picks
    // on the assembly path (see bin_packing.constraints), a name picked
    // several times once per pick.
    m.def(
        "calculate_score",
        [](BinPackingProblem &problem, const py::object &constraints)
        {
            if (constraints.is_none())
                return ScoreCalculator::score(problem);
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            calculator.rebuild(problem);
            problem.score = calculator.score();
            return problem.score;
        },
        py::arg("problem"),
        py::arg("constraints") = py::none(),
        "Full recompute of the HardSoftScore (also stores problem.score).");

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
        // 0 = auto: the solver scales the history with the instance size
        // (clamp(8 * items, 400, 3200)).
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
        // ruin_recreate.as_move_weight). 0 disables it.
        .def_readwrite("ruin_recreate_as_move_weight", &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin", &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        // Guided change-move targets: the bin of a random other item (a bin
        // in use). probability 0 = off (uniform over all n bins).
        .def_readwrite("value_recommender_probability", &Solver::Config::valueRecommenderProbability)
        // BASIC best-improvement descent strategy weight. 0 = off.
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
        // Gate on how often a step may draw a macro move (EmptyBinMove,
        // RepackMove). 1 = engine default (unrestricted).
        .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
        // EmptyBinMove: the domain macro move that empties one light bin in a
        // single step (see cpp/include/bp/moves.hpp).
        .def_readwrite("empty_bin_moves", &Solver::Config::emptyBinMoves)
        .def_property(
            "empty_bin_candidates",
            [](const Solver::Config &c) { return c.emptyBinCandidates; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error("empty_bin_candidates must be positive");
                c.emptyBinCandidates = count;
            })
        // RepackMove: empty a light bin with up to repack_partner_bins other
        // light bins and re-place their items best fit decreasing without a
        // capacity, colour or conflict clash (cpp/include/bp/moves.hpp).
        .def_readwrite("repack_moves", &Solver::Config::repackMoves)
        .def_property(
            "repack_partner_bins",
            [](const Solver::Config &c) { return c.repackPartnerBins; },
            [](Solver::Config &c, int count)
            {
                if (count < 0)
                    throw py::value_error("repack_partner_bins must be >= 0");
                c.repackPartnerBins = count;
            })
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c) { return py::make_tuple(c.target_score.hard, c.target_score.soft); },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.target_score = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // parallel workers too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets item bins, problem.score, clears warm_start.
        // The GIL is released so parallel workers can fire on_new_best.
        .def("solve",
             py::overload_cast<BinPackingProblem &>(&Solver::solve),
             py::arg("problem"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // bin_packing.constraints). The picks replace the defaults; None
        // keeps them. Single start (parallel_solver_count is ignored). A
        // name may be picked several times: every pick is its own instance
        // (a fresh copy of the default component) with its own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver, BinPackingProblem &problem, const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
                py::gil_scoped_release release;
                solver.solve(problem, calculator);
            },
            py::arg("problem"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `problem` — the input
    // bin_packing.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](BinPackingProblem &problem, const py::object &constraints)
        {
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            return tfpy::constraint_costs(calculator, problem);
        },
        py::arg("problem"),
        py::arg("constraints") = py::none(),
        "Raw per-bin cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to bin_packing.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
