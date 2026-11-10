// pybind11 bindings for the knapsack C++ domain.
// The Python extension links the ks::* implementations and imports the
// generic optimaru engine from optimaru/cpp/optimaru; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::map / std::optional conversions

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ks/constraints.hpp"
#include "ks/construction.hpp"
#include "ks/domain.hpp"
#include "ks/score_calculator.hpp"
#include "ks/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace ks;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() = "Knapsack — optimaru engine-backed C++ wrapper exposed to Python";

    // Every class is module_local: the server's isolated runtime loads several
    // domains whose classes share names (Score, Settings, Solver).
    py::class_<Score>(m, "Score", py::module_local())
            .def_readonly("hard", &Score::hard)
            .def_readonly("soft", &Score::soft)
            .def("is_feasible", &Score::feasible)
            .def("__str__", &Score::str)
            .def("__repr__", [](const Score& s) { return "<Score " + s.str() + ">"; });

    py::class_<Resource>(m, "Resource", py::module_local())
            .def(py::init([](std::string id, std::int64_t capacity) {
                     return Resource{std::move(id), capacity};
                 }),
                 py::arg("id"), py::arg("capacity"))
            .def_readwrite("id", &Resource::id)
            .def_readwrite("capacity", &Resource::capacity);

    py::class_<Item>(m, "Item", py::module_local())
            .def(py::init([](std::string id, std::int64_t weight, std::int64_t value,
                             std::map<std::string, std::int64_t> resourceUsage,
                             std::vector<int> requiredItems, int selected) {
                     Item item;
                     item.id = std::move(id);
                     item.weight = weight;
                     item.value = value;
                     item.resourceUsage = std::move(resourceUsage);
                     item.requiredItems = std::move(requiredItems);
                     item.selected = selected;
                     return item;
                 }),
                 py::arg("id"), py::arg("weight"), py::arg("value"),
                 py::arg("resource_usage") = std::map<std::string, std::int64_t>{},
                 py::arg("required_items") = std::vector<int>{}, py::arg("selected") = kNotSelected)
            .def_readwrite("id", &Item::id)
            .def_readwrite("weight", &Item::weight)
            .def_readwrite("value", &Item::value)
            .def_readwrite("resource_usage", &Item::resourceUsage)
            // Item indices (the model's `requires`, by id).
            .def_readwrite("required_items", &Item::requiredItems)
            // The decision variable: 1 = selected, 0 = not, -1 = undecided.
            .def_readwrite("selected", &Item::selected)
            .def("is_selected", &Item::isSelected);

    py::class_<SelectionGroup>(m, "SelectionGroup", py::module_local())
            .def(py::init([](std::string id, std::vector<int> items, int minSelected,
                             std::optional<int> maxSelected) {
                     return SelectionGroup{std::move(id), std::move(items), minSelected,
                                           maxSelected};
                 }),
                 py::arg("id"), py::arg("items"), py::arg("min_selected") = 0,
                 py::arg("max_selected") = py::none())
            .def_readwrite("id", &SelectionGroup::id)
            .def_readwrite("items", &SelectionGroup::items)
            .def_readwrite("min_selected", &SelectionGroup::minSelected)
            // None = the group size.
            .def_readwrite("max_selected", &SelectionGroup::maxSelected);

    py::class_<KnapsackProblem>(m, "KnapsackProblem", py::module_local())
            .def(py::init([](std::int64_t capacity, std::vector<Item> items,
                             std::vector<Resource> resources, std::vector<int> mandatoryItems,
                             std::vector<int> forbiddenItems,
                             std::vector<SelectionGroup> selectionGroups, bool warmStart) {
                     KnapsackProblem problem;
                     problem.capacity = capacity;
                     problem.items = std::move(items);
                     problem.resources = std::move(resources);
                     problem.mandatoryItems = std::move(mandatoryItems);
                     problem.forbiddenItems = std::move(forbiddenItems);
                     problem.selectionGroups = std::move(selectionGroups);
                     problem.warmStart = warmStart;
                     return problem;
                 }),
                 py::arg("capacity"), py::arg("items"),
                 py::arg("resources") = std::vector<Resource>{},
                 py::arg("mandatory_items") = std::vector<int>{},
                 py::arg("forbidden_items") = std::vector<int>{},
                 py::arg("selection_groups") = std::vector<SelectionGroup>{},
                 py::arg("warm_start") = false)
            // NOTE: element access returns copies; use set_assignment for bulk
            // selection writes.
            .def_readwrite("capacity", &KnapsackProblem::capacity)
            .def_readwrite("items", &KnapsackProblem::items)
            // In declaration order (the model's resource_capacities dict order).
            .def_readwrite("resources", &KnapsackProblem::resources)
            .def_readwrite("mandatory_items", &KnapsackProblem::mandatoryItems)
            .def_readwrite("forbidden_items", &KnapsackProblem::forbiddenItems)
            .def_readwrite("selection_groups", &KnapsackProblem::selectionGroups)
            // Start the next solve from the current selection (undecided items go
            // through the greedy construction); the solve resets it to False.
            .def_readwrite("warm_start", &KnapsackProblem::warmStart)
            // No weight fields: weights come only from constraint selections; the
            // defaults are the components' kDefaultWeight (constraint_info()).
            .def_readonly("score", &KnapsackProblem::score)
            .def("item_count", &KnapsackProblem::itemCount)
            .def("total_weight", &KnapsackProblem::totalWeight)
            .def("total_value", &KnapsackProblem::totalValue)
            .def("assignment", &KnapsackProblem::assignment)
            .def(
                    "set_assignment",
                    [](KnapsackProblem& problem, const std::vector<int>& selected) {
                        if (selected.size() != problem.items.size())
                            throw py::value_error("assignment length must equal the item count");
                        for (std::size_t i = 0; i < selected.size(); ++i) {
                            if (selected[i] < kUndecided || selected[i] > kSelected) {
                                throw py::value_error("item '" + problem.items[i].id +
                                                      "': selected must be -1, 0 or 1, not " +
                                                      std::to_string(selected[i]));
                            }
                        }
                        for (std::size_t i = 0; i < selected.size(); ++i)
                            problem.items[i].selected = selected[i];
                    },
                    py::arg("assignment"));

    m.def(
            "calculate_score",
            [](KnapsackProblem& problem) { return ScoreCalculator::score(problem); },
            py::arg("problem"),
            "Full recompute of the score at the default constraints (also "
            "stores problem.score).");

    m.def(
            "greedy_selection",
            [](KnapsackProblem& problem, const std::vector<int>& fixed) {
                ScoreCalculator::prepare(problem);
                std::vector<bool> out;
                for (int value : greedySelection(problem, fixed)) out.push_back(value == kSelected);
                return out;
            },
            py::arg("problem"), py::arg("fixed"),
            "The greedy construction: items by descending value / weight (ties in "
            "item order) while the weight fits. `fixed` holds one entry per item: "
            "0 / 1 kept as given (the load starts at the items fixed to 1), -1 "
            "decided by the greedy. Returns one bool per item.");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
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
            .def_readwrite("ruin_recreate_polish_steps", &Solver::Config::ruinRecreatePolishSteps)
            // Destroy-and-repair as an ordinary acceptor-judged move (engine
            // ruin_recreate.as_move_weight). 0 disables it.
            .def_readwrite("ruin_recreate_as_move_weight",
                           &Solver::Config::ruinRecreateAsMoveWeight)
            .def_readwrite("ruin_recreate_as_move_max_ruin",
                           &Solver::Config::ruinRecreateAsMoveMaxRuin)
            // A change move flips one item; a swap move trades a selected item
            // for an unselected one.
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Assignment best-improvement descent strategy weight. 0 = off.
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            // Nearby swap partners: items of similar weight. probability 0 = off.
            .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
            .def_readwrite("nearby_size", &Solver::Config::nearbySize)
            // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
            // stops at the first feasible solution.
            .def_property(
                    "best_score_limit",
                    [](const Solver::Config& c) {
                        return py::make_tuple(c.target_score.hard, c.target_score.soft);
                    },
                    [](Solver::Config& c, std::pair<std::int64_t, double> hs) {
                        c.target_score = Score{hs.first, hs.second};
                    })
            // Callback fired on every new best solution: on_new_best(step, score).
            // The pybind11 wrapper re-acquires the GIL, so it is safe from the
            // multi-start worker threads too.
            .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: constructs, searches, sets item.selected,
            // problem.score and warm_start = False. The GIL is released so
            // parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<KnapsackProblem&>(&Solver::solve), py::arg("problem"),
                 py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.knapsack.constraints). The picks replace the defaults; None keeps
            // them. Single start. A name may be picked several times: every pick
            // is its own instance (a fresh copy of the default component) with
            // its own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, KnapsackProblem& problem, const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // The raw costs of an assembly on `problem` — the input
    // optimaru.knapsack.constraints.score() turns into a score. Every knapsack
    // constraint is global: one raw total each. None = the default assembly
    // in list_constraints() order; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
            "constraint_costs",
            [](KnapsackProblem& problem, const py::object& constraints) {
                ScoreCalculator::prepare(problem);
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                return opy::constraint_costs(calculator, problem);
            },
            py::arg("problem"), py::arg("constraints") = py::none(),
            "Raw cost of every constraint (one total each), before weight/exponent/offset/level.");

    // The constraint names this domain scores, in catalog order — the same
    // strings a score breakdown reports.
    m.def(
            "list_constraints", [] { return opy::constraint_names(defaultConstraints()); },
            "Constraint names available to optimaru.knapsack.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    opy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
