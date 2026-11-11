// pybind11 bindings for the bin-packing C++ domain.
// The Python extension links the bp::* implementations and imports the
// generic optimaru engine from optimaru/cpp/optimaru; the engine itself is not
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
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace bp;
namespace opy = optimaru::python;

namespace {

std::string itemRepr(const Item& item) {
    std::string color = item.color ? py::repr(py::str(*item.color)).cast<std::string>() : "None";
    return "Item(id=" + py::repr(py::str(item.id)).cast<std::string>() +
           ", weight=" + std::to_string(item.weight) + ", bin=" + std::to_string(item.bin) +
           ", color=" + color + ")";
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Bin packing — optimaru engine-backed C++ wrapper exposed to Python";

    opy::bind_score<Score>(m)
            .def(py::init([](std::int64_t hard, double soft) { return Score{hard, soft}; }),
                 py::arg("hard") = 0, py::arg("soft") = 0.0)
            // Lexicographic (hard first), the engine's own ordering.
            .def(py::self == py::self)
            .def("__ne__", [](const Score& a, const Score& b) { return !(a == b); })
            .def(py::self < py::self)
            .def(py::self <= py::self)
            .def(py::self > py::self)
            .def(py::self >= py::self)
            .def("__hash__",
                 [](const Score& s) { return py::hash(py::make_tuple(s.hard, s.soft)); });

    py::class_<Item>(m, "Item", py::module_local())
            .def(py::init([](std::string id, std::int64_t weight, int bin,
                             std::optional<std::string> color) {
                     Item item;
                     item.id = std::move(id);
                     item.weight = weight;
                     item.bin = bin;
                     item.color = std::move(color);
                     return item;
                 }),
                 py::arg("id"), py::arg("weight"), py::arg("bin") = -1,
                 py::arg("color") = py::none())
            .def_readwrite("id", &Item::id)
            .def_readwrite("weight", &Item::weight)
            // The decision variable: a 0-based bin index, -1 == unassigned.
            .def_readwrite("bin", &Item::bin)
            .def_readwrite("color", &Item::color)
            .def("is_assigned", &Item::isAssigned)
            .def("__repr__", &itemRepr);

    py::class_<BinPackingProblem>(m, "BinPackingProblem", py::module_local())
            .def(py::init([](std::int64_t binCapacity, std::vector<Item> items,
                             std::vector<std::pair<std::string, std::string>> conflicts,
                             bool warmStart) {
                     BinPackingProblem problem;
                     problem.binCapacity = binCapacity;
                     problem.items = std::move(items);
                     problem.conflicts = std::move(conflicts);
                     problem.warmStart = warmStart;
                     return problem;
                 }),
                 py::arg("bin_capacity"), py::arg("items"),
                 py::arg("conflicts") = std::vector<std::pair<std::string, std::string>>{},
                 py::arg("warm_start") = false)
            .def_readwrite("bin_capacity", &BinPackingProblem::binCapacity)
            // NOTE: element access returns copies (item.bin = x on
            // problem.items[i] is lost); use set_assignment for bin writes.
            // Replacing the list (or `conflicts`) rebuilds the derived lookups at
            // the next scoring / solving call.
            .def_property(
                    "items", [](const BinPackingProblem& problem) { return problem.items; },
                    [](BinPackingProblem& problem, std::vector<Item> items) {
                        problem.items = std::move(items);
                        problem.prepared = false;
                    })
            .def_property(
                    "conflicts", [](const BinPackingProblem& problem) { return problem.conflicts; },
                    [](BinPackingProblem& problem,
                       std::vector<std::pair<std::string, std::string>> conflicts) {
                        problem.conflicts = std::move(conflicts);
                        problem.prepared = false;
                    })
            // true = the next solve starts from the items' bins (a seed); every
            // solve clears it.
            .def_readwrite("warm_start", &BinPackingProblem::warmStart)
            // No weight fields: weights come only from
            // constraint selections; the defaults are the components'
            // kDefaultWeight, reported by constraint_info().
            .def_readonly("score", &BinPackingProblem::score)
            .def("item_count", &BinPackingProblem::itemCount)
            .def("bins_used", &BinPackingProblem::binsUsed)
            .def("bin_loads", &BinPackingProblem::binLoads)
            // Convenience: the current bin per item (the solution).
            .def("assignment", &BinPackingProblem::assignment)
            // Bulk write of the decision variable (item access via .items
            // returns copies, so per-item writes would be lost).
            .def(
                    "set_assignment",
                    [](BinPackingProblem& problem, const std::vector<int>& bins) {
                        if (bins.size() != problem.items.size())
                            throw py::value_error("assignment length must equal the item count");
                        problem.setAssignment(bins);
                    },
                    py::arg("assignment"));

    // Full recompute of the Score (also stores problem.score).
    // constraints=None scores the default set (the monolithic calculator,
    // which knows the default picks only); a list scores exactly those picks
    // on the assembly path (see optimaru.bin_packing.constraints), a name picked
    // several times once per pick.
    m.def(
            "calculate_score",
            [](BinPackingProblem& problem, const py::object& constraints) {
                if (constraints.is_none()) return ScoreCalculator::score(problem);
                ScoreCalculator::prepare(problem);
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                calculator.rebuild(problem);
                problem.score = calculator.score();
                return problem.score;
            },
            py::arg("problem"), py::arg("constraints") = py::none(),
            "Full recompute of the score (also stores problem.score).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Guided change-move targets: the bin of a random other item (a bin
            // in use). probability 0 = off (uniform over all n bins).
            .def_readwrite("value_recommender_probability",
                           &Solver::Config::value_recommender_probability)
            // Assignment best-improvement descent strategy weight. 0 = off.
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            // Gate on how often a step may draw a macro move (EmptyBinMove,
            // RepackMove, TradeMove). 1 = engine default (unrestricted).
            .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
            // EmptyBinMove: the domain macro move that empties one light bin in a
            // single step (see cpp/include/bp/moves.hpp).
            .def_readwrite("empty_bin_moves", &Solver::Config::empty_bin_moves)
            .def_property(
                    "empty_bin_candidates",
                    [](const Solver::Config& c) { return c.empty_bin_candidates; },
                    [](Solver::Config& c, int count) {
                        if (count <= 0)
                            throw py::value_error("empty_bin_candidates must be positive");
                        c.empty_bin_candidates = count;
                    })
            // RepackMove: empty a light bin with up to repack_partner_bins other
            // light bins and re-place their items best fit decreasing without a
            // capacity, colour or conflict clash (cpp/include/bp/moves.hpp).
            .def_readwrite("repack_moves", &Solver::Config::repack_moves)
            // TradeMove: a trade of up to two items a side between two bins that
            // leaves the fuller one fuller (cpp/include/bp/moves.hpp).
            .def_readwrite("trade_moves", &Solver::Config::trade_moves)
            // Steps that keep the score: taken when the bins get no less full,
            // else one time in eight (cpp/include/bp/fill_late_acceptance.hpp).
            .def_readwrite("fill_tie_break", &Solver::Config::fill_tie_break)
            .def_property(
                    "repack_partner_bins",
                    [](const Solver::Config& c) { return c.repack_partner_bins; },
                    [](Solver::Config& c, int count) {
                        if (count < 0) throw py::value_error("repack_partner_bins must be >= 0");
                        c.repack_partner_bins = count;
                    });

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets item bins, problem.score, clears warm_start.
            // The GIL is released so parallel workers can fire on_new_best.
            .def("solve", py::overload_cast<BinPackingProblem&>(&Solver::solve), py::arg("problem"),
                 py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.bin_packing.constraints). The picks replace the defaults; None
            // keeps them. Single start (parallel_solver_count is ignored). A
            // name may be picked several times: every pick is its own instance
            // (a fresh copy of the default component) with its own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, BinPackingProblem& problem, const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly. The units of constraint_costs() are the bins.
    opy::bind_constraints<BinPackingProblem>(
            m, "problem", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
