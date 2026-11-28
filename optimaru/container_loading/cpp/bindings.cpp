// pybind11 bindings for the container-loading C++ core.
// The Python extension links the cl::* implementations (cl/ beside this file) and imports
// the generic optimaru engine header-only from core/optimaru.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional conversions

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cl/constraints.hpp"
#include "cl/model/demo_data.hpp"
#include "cl/model/domain.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace cl;
namespace opy = optimaru::python;

namespace {

std::string quoted(const std::string& text) { return py::repr(py::str(text)).cast<std::string>(); }

std::string itemRepr(const Item& item) {
    return "Item(id=" + quoted(item.id) + ", length=" + std::to_string(item.length) +
           ", width=" + std::to_string(item.width) + ", height=" + std::to_string(item.height) +
           ", weight=" + std::to_string(item.weight) + ")";
}

std::string containerRepr(const Container& container) {
    return "Container(id=" + quoted(container.id) + ", length=" + std::to_string(container.length) +
           ", width=" + std::to_string(container.width) +
           ", height=" + std::to_string(container.height) +
           ", items=" + std::to_string(container.items.size()) + ")";
}

std::vector<StackingRule> toRules(const std::vector<std::pair<std::string, std::string>>& rules) {
    std::vector<StackingRule> out;
    for (const auto& [top, bottom] : rules) out.push_back({top, bottom});
    return out;
}

using GroupRuleTuple = std::tuple<std::string, bool, bool>;

std::vector<GroupRule> toGroupRules(const std::vector<GroupRuleTuple>& rules) {
    std::vector<GroupRule> out;
    for (const auto& [id, allOrNone, singleContainer] : rules)
        out.push_back({id, allOrNone, singleContainer});
    return out;
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Container loading — C++ core exposed to Python";

    opy::bind_score<Score>(m);

    py::enum_<Rotation>(m, "Rotation", py::module_local())
            .value("ALL", Rotation::All)
            .value("UPRIGHT", Rotation::Upright)
            .value("FIXED", Rotation::Fixed);

    py::class_<Item>(m, "Item", py::module_local())
            .def(py::init([](std::string id, int length, int width, int height, std::int64_t weight,
                             Rotation rotations, std::optional<std::int64_t> maxLoadOnTop,
                             std::optional<int> unloadOrder, std::optional<std::string> group,
                             bool bottomOnly, std::optional<int> maxStackLayers,
                             std::optional<std::string> stackGroup, int priority, bool optional) {
                     Item item;
                     item.id = std::move(id);
                     item.length = length;
                     item.width = width;
                     item.height = height;
                     item.weight = weight;
                     item.rotations = rotations;
                     item.maxLoadOnTop = maxLoadOnTop;
                     item.unloadOrder = unloadOrder;
                     item.group = std::move(group);
                     item.bottomOnly = bottomOnly;
                     item.maxStackLayers = maxStackLayers;
                     item.stackGroup = std::move(stackGroup);
                     item.priority = priority;
                     item.optional = optional;
                     return item;
                 }),
                 py::arg("id"), py::arg("length"), py::arg("width"), py::arg("height"),
                 py::arg("weight") = 0, py::arg("rotations") = Rotation::All,
                 py::arg("max_load_on_top") = py::none(), py::arg("unload_order") = py::none(),
                 py::arg("group") = py::none(), py::arg("bottom_only") = false,
                 py::arg("max_stack_layers") = py::none(), py::arg("stack_group") = py::none(),
                 py::arg("priority") = 1, py::arg("optional") = false)
            .def_readwrite("id", &Item::id)
            .def_readwrite("length", &Item::length)
            .def_readwrite("width", &Item::width)
            .def_readwrite("height", &Item::height)
            .def_readwrite("weight", &Item::weight)
            .def_readwrite("rotations", &Item::rotations)
            .def_readwrite("max_load_on_top", &Item::maxLoadOnTop)
            .def_readwrite("unload_order", &Item::unloadOrder)
            .def_readwrite("group", &Item::group)
            .def_readwrite("bottom_only", &Item::bottomOnly)
            .def_readwrite("max_stack_layers", &Item::maxStackLayers)
            .def_readwrite("stack_group", &Item::stackGroup)
            .def_readwrite("priority", &Item::priority)
            .def_readwrite("optional", &Item::optional)
            // Derived (refreshed by calculate_score / solve): the container
            // index (-1 = not placed), the place among its placed items, the
            // corner nearest the origin and the size after rotation.
            .def_readonly("container", &Item::container)
            .def_readonly("sequence", &Item::sequence)
            .def_readonly("x", &Item::x)
            .def_readonly("y", &Item::y)
            .def_readonly("z", &Item::z)
            .def_readonly("placed_length", &Item::placedLength)
            .def_readonly("placed_width", &Item::placedWidth)
            .def_readonly("placed_height", &Item::placedHeight)
            .def("is_placed", &Item::isPlaced)
            .def("__repr__", &itemRepr);

    py::class_<Container>(m, "Container", py::module_local())
            .def(py::init([](std::string id, int length, int width, int height,
                             std::optional<std::int64_t> maxWeight, double cost,
                             std::vector<int> items, std::optional<int> maxStackHeight,
                             std::optional<double> maxOffsetLength,
                             std::optional<double> maxOffsetWidth) {
                     Container container;
                     container.id = std::move(id);
                     container.length = length;
                     container.width = width;
                     container.height = height;
                     container.maxWeight = maxWeight;
                     container.cost = cost;
                     container.items = std::move(items);
                     container.maxStackHeight = maxStackHeight;
                     container.maxOffsetLength = maxOffsetLength;
                     container.maxOffsetWidth = maxOffsetWidth;
                     return container;
                 }),
                 py::arg("id"), py::arg("length"), py::arg("width"), py::arg("height"),
                 py::arg("max_weight") = py::none(), py::arg("cost") = 1.0,
                 py::arg("items") = std::vector<int>{}, py::arg("max_stack_height") = py::none(),
                 py::arg("max_offset_length") = py::none(),
                 py::arg("max_offset_width") = py::none())
            .def_readwrite("id", &Container::id)
            .def_readwrite("length", &Container::length)
            .def_readwrite("width", &Container::width)
            .def_readwrite("height", &Container::height)
            .def_readwrite("max_weight", &Container::maxWeight)
            .def_readwrite("cost", &Container::cost)
            .def_readwrite("max_stack_height", &Container::maxStackHeight)
            .def_readwrite("max_offset_length", &Container::maxOffsetLength)
            .def_readwrite("max_offset_width", &Container::maxOffsetWidth)
            // The decision: the item indices in loading order.
            .def_readwrite("items", &Container::items)
            // Derived (refreshed by calculate_score / solve).
            .def_readonly("placed_items", &Container::placedItems)
            .def_readonly("unplaced_items", &Container::unplacedItems)
            .def_readonly("loaded_weight", &Container::loadedWeight)
            .def_readonly("loaded_volume", &Container::loadedVolume)
            .def_readonly("used_length", &Container::usedLength)
            .def_property_readonly("fill_ratio", &Container::fillRatio)
            .def("__repr__", &containerRepr);

    py::class_<ContainerLoadingProblem>(m, "ContainerLoadingProblem", py::module_local())
            .def(py::init([](std::vector<Container> containers, std::vector<Item> items,
                             double minSupportRatio,
                             const std::vector<std::pair<std::string, std::string>>& rules,
                             const std::vector<GroupRuleTuple>& groupRules) {
                     ContainerLoadingProblem problem;
                     problem.containers = std::move(containers);
                     problem.items = std::move(items);
                     problem.minSupportRatio = minSupportRatio;
                     problem.stackingRules = toRules(rules);
                     problem.groupRules = toGroupRules(groupRules);
                     ScoreCalculator::prepare(problem);  // refuses a bad problem here
                     return problem;
                 }),
                 py::arg("containers"), py::arg("items"), py::arg("min_support_ratio") = 0.8,
                 py::arg("stacking_rules") = std::vector<std::pair<std::string, std::string>>{},
                 py::arg("group_rules") = std::vector<GroupRuleTuple>{})
            // NOTE: element access returns copies (a write to
            // problem.containers[i].items is lost); use set_loading for the
            // loading orders, or assign a whole list back.
            .def_property(
                    "containers",
                    [](const ContainerLoadingProblem& problem) { return problem.containers; },
                    [](ContainerLoadingProblem& problem, std::vector<Container> containers) {
                        problem.containers = std::move(containers);
                    })
            .def_property(
                    "items", [](const ContainerLoadingProblem& problem) { return problem.items; },
                    [](ContainerLoadingProblem& problem, std::vector<Item> items) {
                        problem.items = std::move(items);
                    })
            .def_readwrite("min_support_ratio", &ContainerLoadingProblem::minSupportRatio)
            // (top, bottom): items of stack group `top` may not stand on items of `bottom`.
            .def_property(
                    "stacking_rules",
                    [](const ContainerLoadingProblem& problem) {
                        std::vector<std::pair<std::string, std::string>> rules;
                        for (const StackingRule& rule : problem.stackingRules)
                            rules.emplace_back(rule.top, rule.bottom);
                        return rules;
                    },
                    [](ContainerLoadingProblem& problem,
                       const std::vector<std::pair<std::string, std::string>>& rules) {
                        problem.stackingRules = toRules(rules);
                    })
            // (group, all_or_none, single_container): how a group's items are loaded.
            .def_property(
                    "group_rules",
                    [](const ContainerLoadingProblem& problem) {
                        std::vector<GroupRuleTuple> rules;
                        for (const GroupRule& rule : problem.groupRules)
                            rules.emplace_back(rule.id, rule.allOrNone, rule.singleContainer);
                        return rules;
                    },
                    [](ContainerLoadingProblem& problem, const std::vector<GroupRuleTuple>& rules) {
                        problem.groupRules = toGroupRules(rules);
                    })
            .def_readonly("score", &ContainerLoadingProblem::score)
            // The decision: per container, the item indices in loading order.
            .def("loading",
                 [](const ContainerLoadingProblem& problem) {
                     std::vector<std::vector<int>> lists;
                     for (const Container& container : problem.containers)
                         lists.push_back(container.items);
                     return lists;
                 })
            .def(
                    "set_loading",
                    [](ContainerLoadingProblem& problem,
                       const std::vector<std::vector<int>>& lists) {
                        if (lists.size() != problem.containers.size())
                            throw py::value_error("loading must hold one list per container");
                        std::vector<char> listed(problem.items.size(), 0);
                        for (const std::vector<int>& list : lists) {
                            for (int item : list) {
                                if (item < 0 || item >= static_cast<int>(problem.items.size()))
                                    throw py::value_error(
                                            "loading holds an item index out of range");
                                if (listed[static_cast<std::size_t>(item)])
                                    throw py::value_error("loading holds an item more than once");
                                listed[static_cast<std::size_t>(item)] = 1;
                            }
                        }
                        for (std::size_t c = 0; c < lists.size(); ++c)
                            problem.containers[c].items = lists[c];
                    },
                    py::arg("loading"))
            // Derived (as of the last calculate_score / solve).
            .def("containers_used", &ContainerLoadingProblem::containersUsed)
            .def("unplaced_items", [](const ContainerLoadingProblem& problem) {
                std::vector<int> unplaced;
                for (std::size_t i = 0; i < problem.items.size(); ++i) {
                    if (!problem.items[i].isPlaced()) unplaced.push_back(static_cast<int>(i));
                }
                return unplaced;
            });

    m.def(
            "calculate_score",
            [](ContainerLoadingProblem& problem) { return ScoreCalculator::score(problem); },
            py::arg("problem"),
            "Decode every container, write where each item stands and recompute the hard / soft "
            "score at the default weights (also stores problem.score).");

    m.def(
            "update_placements",
            [](ContainerLoadingProblem& problem) {
                ScoreCalculator::prepare(problem);
                ScoreCalculator::updateDerived(problem);
            },
            py::arg("problem"),
            "Decode every container and write where each item stands (the derived fields of the "
            "items and containers), leaving problem.score as it is.");

    m.def("generate_demo_data", &generateDemoData, py::arg("item_count") = 40,
          py::arg("container_count") = 4, py::arg("random_seed") = 37,
          "The demo pallets (deterministic for a given random_seed).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            // Largest items first, each into the first container that takes it
            // (cpp/cl/construct/construction.hpp); False = cheapest insertion.
            .def_readwrite("first_fit_decreasing", &Solver::Config::first_fit_decreasing)
            .def_readwrite("relocate_move_weight", &Solver::Config::relocate_weight)
            .def_readwrite("list_swap_move_weight", &Solver::Config::list_swap_weight)
            .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
            .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
            // EmptyContainerMove: empty one of the least filled containers into
            // the others in one move (cpp/cl/search/moves/empty_container.hpp).
            .def_readwrite("empty_container_moves", &Solver::Config::empty_container_moves)
            .def_property(
                    "empty_container_candidates",
                    [](const Solver::Config& c) { return c.empty_container_candidates; },
                    [](Solver::Config& c, int count) {
                        if (count <= 0)
                            throw py::value_error("empty_container_candidates must be positive");
                        c.empty_container_candidates = count;
                    })
            // RepackPairMove: two containers loaded again, group by group and largest
            // first (cpp/cl/search/moves/repack_pair.hpp).
            .def_readwrite("repack_moves", &Solver::Config::repack_moves)
            // SwitchContainerMove: a load into empty containers of another size or
            // cost (cpp/cl/search/moves/switch_container.hpp).
            .def_readwrite("switch_container_moves", &Solver::Config::switch_container_moves)
            // StopOrderMove: a loading order put back in stop blocks, with one
            // change inside a block (cpp/cl/search/moves/stop_order.hpp).
            .def_readwrite("stop_order_moves", &Solver::Config::stop_order_moves)
            // Nearby selection by stop, only with unloading orders.
            .def_readwrite("nearby_probability", &Solver::Config::nearby_probability)
            .def_readwrite("nearby_size", &Solver::Config::nearby_size);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets the loading orders, the derived fields and
            // problem.score. The GIL is released so parallel workers can fire
            // on_new_best.
            .def("solve", py::overload_cast<ContainerLoadingProblem&>(&Solver::solve),
                 py::arg("problem"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.container_loading.constraints). The picks replace the
            // defaults; None keeps them. Single start (parallel_solver_count is
            // ignored). A name may be picked several times: every pick is its
            // own instance (a fresh copy of the default component) with its own
            // knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, ContainerLoadingProblem& problem,
                       const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly. The units of constraint_costs() are the containers.
    opy::bind_constraints<ContainerLoadingProblem>(
            m, "problem", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
