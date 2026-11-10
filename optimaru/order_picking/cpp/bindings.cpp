// pybind11 bindings for the order-picking C++ core.
// Wraps the existing op::* use case (include/ and src/ beside this file) without modifying it: one
// solver, one module (optimaru.order_picking._core). The generic optimaru engine is
// imported header-only from optimaru/cpp/optimaru.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "op/constraints.hpp"
#include "op/demo_data.hpp"
#include "op/domain.hpp"
#include "op/incremental_calculator.hpp"
#include "op/score_calculator.hpp"
#include "op/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace op;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Order picking — C++ core exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::enum_<Side>(m, "Side", py::module_local())
        .value("LEFT", Side::LEFT)
        .value("RIGHT", Side::RIGHT);

    py::class_<WarehouseLocation>(m, "WarehouseLocation", py::module_local())
        .def(py::init(
                 [](int column, int shelvingRow, Side side, int row)
                 {
                     if (column < 0 || column >= Warehouse::COLUMN_COUNT)
                         throw py::value_error(
                             "column must be 0..4 (shelving columns A..E)");
                     if (shelvingRow < 0 || shelvingRow >= Warehouse::ROW_COUNT)
                         throw py::value_error(
                             "shelving_row must be 0..2 (shelving rows 1..3)");
                     if (row < 0 || row > Warehouse::SHELVING_HEIGHT)
                         throw py::value_error("row must be 0..10");
                     WarehouseLocation location;
                     location.column = column;
                     location.shelvingRow = shelvingRow;
                     location.side = side;
                     location.row = row;
                     return location;
                 }),
             py::arg("column"),
             py::arg("shelving_row"),
             py::arg("side"),
             py::arg("row"))
        .def_readonly("column", &WarehouseLocation::column)
        .def_readonly("shelving_row", &WarehouseLocation::shelvingRow)
        .def_readonly("side", &WarehouseLocation::side)
        .def_readonly("row", &WarehouseLocation::row)
        // The shelving id, e.g. "(A,1)".
        .def_property_readonly("shelving_id", &WarehouseLocation::shelvingId)
        .def("__repr__",
             [](const WarehouseLocation &l)
             {
                 return "<WarehouseLocation " + l.shelvingId()
                        + (l.side == Side::LEFT ? " LEFT" : " RIGHT") + " row "
                        + std::to_string(l.row) + ">";
             });

    py::class_<Order>(m, "Order", py::module_local())
        .def(py::init([](std::string id) { return Order{std::move(id)}; }),
             py::arg("id"))
        .def_readwrite("id", &Order::id);

    py::class_<PickTask>(m, "PickTask", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int orderIndex,
                    int volume,
                    const WarehouseLocation &location,
                    std::string productName)
                 {
                     if (volume <= 0)
                         throw py::value_error("volume must be positive");
                     PickTask task;
                     task.id = std::move(id);
                     task.orderIndex = orderIndex;
                     task.productName = std::move(productName);
                     task.volume = volume;
                     task.location = location;
                     return task;
                 }),
             py::arg("id"),
             py::arg("order_index"),
             py::arg("volume"),
             py::arg("location"),
             py::arg("product_name") = std::string())
        .def_readwrite("id", &PickTask::id)
        .def_readwrite("order_index", &PickTask::orderIndex)
        .def_readwrite("product_name", &PickTask::productName)
        .def_readwrite("volume", &PickTask::volume)
        .def_readwrite("location", &PickTask::location)
        // Derived fields (indices, -1 == none), refreshed by
        // calculate_score / update_shadow_variables / solve.
        .def_readonly("trolley_index", &PickTask::trolleyIndex)
        .def_readonly("previous_pick_task", &PickTask::previousPickTask)
        .def_readonly("next_pick_task", &PickTask::nextPickTask)
        .def("is_last", &PickTask::isLast);

    py::class_<Trolley>(m, "Trolley", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int bucketCount,
                    int bucketCapacity,
                    const WarehouseLocation &location)
                 {
                     if (bucketCount < 0)
                         throw py::value_error(
                             "bucket_count must be non-negative");
                     if (bucketCapacity <= 0)
                         throw py::value_error(
                             "bucket_capacity must be positive");
                     Trolley trolley;
                     trolley.id = std::move(id);
                     trolley.bucketCount = bucketCount;
                     trolley.bucketCapacity = bucketCapacity;
                     trolley.location = location;
                     return trolley;
                 }),
             py::arg("id"),
             py::arg("bucket_count"),
             py::arg("bucket_capacity"),
             py::arg("location"))
        .def_readwrite("id", &Trolley::id)
        .def_readwrite("bucket_count", &Trolley::bucketCount)
        .def_readwrite("bucket_capacity", &Trolley::bucketCapacity)
        .def_readwrite("location", &Trolley::location)
        // The decision: the ordered pick-task indices.
        .def_readwrite("pick_task_indices", &Trolley::pickTaskIndices);

    py::class_<OrderPickingSolution>(m, "OrderPickingSolution", py::module_local())
        .def(py::init(
                 [](std::vector<Order> orders,
                    std::vector<Trolley> trolleys,
                    std::vector<PickTask> pickTasks)
                 {
                     for (const PickTask &task : pickTasks)
                     {
                         if (task.orderIndex < 0
                             || task.orderIndex
                                    >= static_cast<int>(orders.size()))
                             throw py::value_error(
                                 "pick task '" + task.id
                                 + "' has an out-of-range order_index");
                     }
                     OrderPickingSolution solution;
                     solution.orders = std::move(orders);
                     solution.trolleys = std::move(trolleys);
                     solution.pickTasks = std::move(pickTasks);
                     return solution;
                 }),
             py::arg("orders"),
             py::arg("trolleys"),
             py::arg("pick_tasks"))
        .def_readonly("orders", &OrderPickingSolution::orders)
        .def_readwrite("trolleys", &OrderPickingSolution::trolleys)
        .def_readonly("pick_tasks", &OrderPickingSolution::pickTasks)
        .def_readonly("score", &OrderPickingSolution::score)
        // Convenience: routes as {trolley id: [pick task ids]}.
        .def("routes",
             [](const OrderPickingSolution &solution)
             {
                 py::dict out;
                 for (const Trolley &trolley : solution.trolleys)
                 {
                     py::list route;
                     for (int pickIndex : trolley.pickTaskIndices)
                         route.append(solution.pickTasks[pickIndex].id);
                     out[py::str(trolley.id)] = route;
                 }
                 return out;
             });

    m.def("calculate_distance",
          &calculateDistance,
          py::arg("start"),
          py::arg("end"),
          "Distance in meters between two warehouse locations.");

    m.def(
        "distance_to_travel",
        [](const OrderPickingSolution &solution, int trolleyIndex)
        {
            if (trolleyIndex < 0
                || trolleyIndex >= static_cast<int>(solution.trolleys.size()))
                throw py::value_error("trolley_index out of range");
            return calculateDistanceToTravel(solution,
                                             solution.trolleys[trolleyIndex]);
        },
        py::arg("solution"),
        py::arg("trolley_index"),
        "Round-trip distance of one trolley's current pick list.");

    m.def(
        "update_shadow_variables",
        [](OrderPickingSolution &solution)
        { ScoreCalculator::updateShadowVariables(solution); },
        py::arg("solution"),
        "Re-derive every pick task's trolley_index / previous_pick_task / "
        "next_pick_task from the trolleys' pick lists.");

    m.def(
        "calculate_score",
        [](OrderPickingSolution &solution)
        { return ScoreCalculator::score(solution); },
        py::arg("solution"),
        "Refresh the derived fields and recompute the hard / soft score "
        "(also stores solution.score).");

    // constraint_costs() is the only per-constraint surface.

    m.def("build_demo_data",
          &buildDemoData,
          py::arg("trolleys_count") = 5,
          py::arg("bucket_count") = 4,
          py::arg("bucket_capacity") = 60 * 40 * 20,
          py::arg("orders_count") = 8,
          py::arg("random_seed") = 37,
          "The demo warehouse (deterministic for a given random_seed).");

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
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocate_weight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::list_swap_weight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tail_swap_weight)
        .def_readwrite("distance_nearby", &Solver::Config::distanceNearby)
        .def_readwrite("distance_nearby_probability",
                       &Solver::Config::distanceNearbyProbability)
        .def_readwrite("distance_nearby_size",
                       &Solver::Config::distanceNearbySize)
        .def_readwrite("order_consolidate_moves",
                       &Solver::Config::orderConsolidateMoves)
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
        // Solves in place: mutates trolleys[*].pick_task_indices, refreshes
        // the derived fields and stores the best score found in
        // solution.score. The GIL is released so on_new_best can be fired
        // from the engine's worker threads.
        .def("solve",
             py::overload_cast<OrderPickingSolution &>(&Solver::solve),
             py::arg("solution"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // optimaru.order_picking.constraints). The picks replace the defaults; None
        // keeps them. A name may be picked several times: every pick is its
        // own instance (a fresh copy of the default component) with its own
        // knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               OrderPickingSolution &solution,
               const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); },
                                       constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(solution, calculator);
            },
            py::arg("solution"),
            py::arg("constraints") = py::none());

    // The raw per-trolley costs of an assembly on `solution` — the input
    // optimaru.order_picking.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](OrderPickingSolution &solution, const py::object &constraints)
        {
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, solution);
        },
        py::arg("solution"),
        py::arg("constraints") = py::none(),
        "Raw per-trolley cost of every constraint, before "
        "weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the keys of constraint_costs().
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.order_picking.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
