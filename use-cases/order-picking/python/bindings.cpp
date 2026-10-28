// pybind11 bindings for the order-picking C++ core.
// Wraps the existing op::* use case (../cpp) without modifying it: one
// solver, one module (order_picking._core). The generic optimaru engine is
// imported header-only from ../../../include.
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
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

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
        // The Java shelving id, e.g. "(A,1)".
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
        // Shadow variables (indices, -1 == null), refreshed by
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
        // The planning list variable: the ordered pick-task indices.
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

    py::class_<ConstraintTotals>(m, "ConstraintTotals", py::module_local())
        .def_readonly("required_number_of_buckets",
                      &ConstraintTotals::requiredNumberOfBuckets)
        .def_readonly("distance_from_previous_pick_task",
                      &ConstraintTotals::distanceFromPreviousPickTask)
        .def_readonly("distance_from_last_pick_task_to_path_origin",
                      &ConstraintTotals::distanceFromLastPickTaskToPathOrigin)
        .def_readonly("order_split_by_trolley",
                      &ConstraintTotals::orderSplitByTrolley)
        .def("to_score", &ConstraintTotals::toScore);

    m.def("calculate_distance",
          &calculateDistance,
          py::arg("start"),
          py::arg("end"),
          "Distance in meters between two warehouse locations "
          "(Warehouse.calculateDistance).");

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
        "Round-trip distance of one trolley's current pick list "
        "(Warehouse.calculateDistanceToTravel).");

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
        { return ScoreCalculator::calculateScore(solution); },
        py::arg("solution"),
        "Refresh the shadow variables and recompute the HardSoft score "
        "(also stores solution.score).");

    m.def(
        "calculate_totals",
        [](OrderPickingSolution &solution)
        { return ScoreCalculator::calculateTotals(solution); },
        py::arg("solution"),
        "Per-constraint penalty totals (all positive), matching the four "
        "Java constraints one to one.");

    m.def("build_demo_data",
          &buildDemoData,
          py::arg("trolleys_count") = 5,
          py::arg("bucket_count") = 4,
          py::arg("bucket_capacity") = 60 * 40 * 20,
          py::arg("orders_count") = 8,
          py::arg("random_seed") = 37,
          "The Java quickstart's DemoDataGenerator dataset (bit-exact "
          "java.util.Random replay).");

    py::class_<Solver::Config>(m, "SolverConfig", py::module_local())
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Solver::Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "spent_limit_seconds must be non-negative");
                c.spentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Solver::Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "unimproved_spent_limit_seconds must be non-negative");
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::unimprovedStepCountLimit)
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
        .def_property(
            "parallel_solver_count",
            [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error(
                        "parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // 0 = auto: clamp(4 * pick task count, 200, 1600).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.lateAcceptanceSize; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error(
                        "late_acceptance_size must be >= 0 (0 = auto)");
                c.lateAcceptanceSize = static_cast<std::size_t>(size);
            })
        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c)
            {
                return c.environmentMode
                       == optimaru::EnvironmentMode::FULL_ASSERT;
            },
            [](Solver::Config &c, bool on)
            {
                c.environmentMode = on ? optimaru::EnvironmentMode::FULL_ASSERT
                                       : optimaru::EnvironmentMode::NON_ASSERT;
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruinRecreate)
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
        .def_readwrite("ruin_recreate_rewards",
                       &Solver::Config::ruinRecreateRewards)
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocateMoveWeight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::listSwapMoveWeight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::twoOptMoveWeight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::orOptMoveWeight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tailSwapMoveWeight)
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
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.bestScoreLimit = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: mutates trolleys[*].pick_task_indices, refreshes
        // the shadow variables and stores the best score found in
        // solution.score. The GIL is released so on_new_best can be fired
        // from the engine's worker threads.
        .def("solve",
             py::overload_cast<OrderPickingSolution &>(&Solver::solve),
             py::arg("solution"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // order_picking.constraints). The picks replace the defaults; None
        // keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               OrderPickingSolution &solution,
               const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(solution, calculator);
            },
            py::arg("solution"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `solution` — the input
    // order_picking.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](OrderPickingSolution &solution)
        {
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            return tfpy::constraintCosts(calculator, solution);
        },
        py::arg("solution"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraintNames(defaultConstraints()); },
        "Constraint names available to order_picking.constraints.");
}
