// pybind11 bindings for the pickup-and-delivery C++ core.
// Wraps the pdp::* use case (include/ and src/ beside this file) without modifying it. Compiled
// independently from the other extensions: one solver, one module
// (optimaru.pickup_delivery._core).
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <tuple>

#include "pdp/constraints.hpp"
#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/haversine.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace pdp;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Pickup and delivery (PDPTW) — C++ core exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("medium", &Score::medium)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Location>(m, "Location", py::module_local())
        .def(py::init(
                 [](double latitude, double longitude)
                 {
                     Location l;
                     l.latitude = latitude;
                     l.longitude = longitude;
                     return l;
                 }),
             py::arg("latitude"),
             py::arg("longitude"))
        .def_readwrite("latitude", &Location::latitude)
        .def_readwrite("longitude", &Location::longitude)
        .def_readonly("index", &Location::index);

    py::class_<Node>(m, "Node", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int locationIndex,
                    Seconds minStart,
                    Seconds maxEnd,
                    Seconds service,
                    int deliveryIndex,
                    bool pickup)
                 {
                     Node n;
                     n.id = std::move(id);
                     n.locationIndex = locationIndex;
                     n.minStartTime = minStart;
                     n.maxEndTime = maxEnd;
                     n.serviceDuration = service;
                     n.deliveryIndex = deliveryIndex;
                     n.pickup = pickup;
                     return n;
                 }),
             py::arg("id"),
             py::arg("location_index"),
             py::arg("min_start_time"),
             py::arg("max_end_time"),
             py::arg("service_duration"),
             py::arg("delivery_index"),
             py::arg("pickup"))
        .def_readwrite("id", &Node::id)
        .def_readwrite("location_index", &Node::locationIndex)
        .def_readwrite("min_start_time", &Node::minStartTime)
        .def_readwrite("max_end_time", &Node::maxEndTime)
        .def_readwrite("service_duration", &Node::serviceDuration)
        .def_readwrite("delivery_index", &Node::deliveryIndex)
        .def_readwrite("pickup", &Node::pickup)
        // Shadow variables, recomputed by calculate_score / solve.
        .def_readonly("vehicle_index", &Node::vehicleIndex)
        .def_readonly("arrival_time", &Node::arrivalTime);

    py::class_<Delivery>(m, "Delivery", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int pickupNode,
                    int dropoffNode,
                    int demand,
                    Seconds orderTime,
                    int requiredVehicleIndex)
                 {
                     Delivery d;
                     d.id = std::move(id);
                     d.pickupNodeIndex = pickupNode;
                     d.dropoffNodeIndex = dropoffNode;
                     d.demand = demand;
                     d.orderTime = orderTime;
                     d.requiredVehicleIndex = requiredVehicleIndex;
                     return d;
                 }),
             py::arg("id"),
             py::arg("pickup_node_index"),
             py::arg("dropoff_node_index"),
             py::arg("demand"),
             py::arg("order_time") = 0,
             py::arg("required_vehicle_index") = -1)
        .def_readwrite("id", &Delivery::id)
        .def_readwrite("pickup_node_index", &Delivery::pickupNodeIndex)
        .def_readwrite("dropoff_node_index", &Delivery::dropoffNodeIndex)
        .def_readwrite("demand", &Delivery::demand)
        .def_readwrite("order_time", &Delivery::orderTime)
        .def_readwrite("required_vehicle_index", &Delivery::requiredVehicleIndex);

    py::class_<Vehicle>(m, "Vehicle", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int capacity,
                    int homeLocationIndex,
                    Seconds departureTime,
                    Seconds maxTravelTime,
                    Seconds maxRouteDuration,
                    Seconds maxEndTime,
                    int maxStops,
                    double minLoadRatio,
                    double fixedCost,
                    double maxRouteRadiusMeters,
                    bool returnToDepot)
                 {
                     Vehicle v;
                     v.id = std::move(id);
                     v.capacity = capacity;
                     v.homeLocationIndex = homeLocationIndex;
                     v.departureTime = departureTime;
                     v.maxTravelTime = maxTravelTime;
                     v.maxRouteDuration = maxRouteDuration;
                     v.maxEndTime = maxEndTime;
                     v.maxStops = maxStops;
                     v.minLoadRatio = minLoadRatio;
                     v.fixedCost = fixedCost;
                     v.maxRouteRadiusMeters = maxRouteRadiusMeters;
                     v.returnToDepot = returnToDepot;
                     return v;
                 }),
             py::arg("id"),
             py::arg("capacity"),
             py::arg("home_location_index"),
             py::arg("departure_time") = 0,
             py::arg("max_travel_time") = 0,
             py::arg("max_route_duration") = 0,
             py::arg("max_end_time") = 0,
             py::arg("max_stops") = 0,
             py::arg("min_load_ratio") = 0.0,
             py::arg("fixed_cost") = 0.0,
             py::arg("max_route_radius_meters") = 0.0,
             py::arg("return_to_depot") = false)
        .def_readwrite("id", &Vehicle::id)
        .def_readwrite("capacity", &Vehicle::capacity)
        .def_readwrite("home_location_index", &Vehicle::homeLocationIndex)
        .def_readwrite("departure_time", &Vehicle::departureTime)
        .def_readwrite("max_travel_time", &Vehicle::maxTravelTime)
        .def_readwrite("max_route_duration", &Vehicle::maxRouteDuration)
        .def_readwrite("max_end_time", &Vehicle::maxEndTime)
        .def_readwrite("max_stops", &Vehicle::maxStops)
        .def_readwrite("min_load_ratio", &Vehicle::minLoadRatio)
        .def_readwrite("fixed_cost", &Vehicle::fixedCost)
        .def_readwrite("max_route_radius_meters",
                       &Vehicle::maxRouteRadiusMeters)
        .def_readwrite(
            "return_to_depot",
            &Vehicle::returnToDepot,
            "Whether the route ends back home (default: open route).")
        // The planning list variable: the ordered stop sequence.
        .def_readwrite("node_indices", &Vehicle::nodeIndices)
        .def_readwrite("pinned_node_indices", &Vehicle::pinnedNodeIndices);

    py::class_<PdpRoutePlan>(m, "PdpRoutePlan", py::module_local())
        .def(py::init(
                 [](std::string name,
                    std::vector<Location> locations,
                    std::vector<Vehicle> vehicles,
                    std::vector<Node> nodes,
                    std::vector<Delivery> deliveries,
                    std::optional<std::vector<std::vector<Seconds>>>
                        drivingSeconds)
                 {
                     PdpRoutePlan plan;
                     plan.name = std::move(name);
                     plan.locations = std::move(locations);
                     for (std::size_t i = 0; i < plan.locations.size(); ++i)
                         plan.locations[i].index = static_cast<int>(i);
                     plan.vehicles = std::move(vehicles);
                     plan.nodes = std::move(nodes);
                     plan.deliveries = std::move(deliveries);
                     if (drivingSeconds)
                     {
                         const std::size_t n = plan.locations.size();
                         if (drivingSeconds->size() != n)
                             throw std::invalid_argument(
                                 "driving_seconds must be a square matrix over "
                                 "locations");
                         for (const auto &row : *drivingSeconds)
                         {
                             if (row.size() != n)
                                 throw std::invalid_argument(
                                     "driving_seconds must be a square matrix "
                                     "over locations");
                         }
                         plan.drivingSeconds = std::move(*drivingSeconds);
                     }
                     else
                     {
                         HaversineDrivingTimeCalculator::initDrivingTimeMatrix(
                             plan);
                     }
                     return plan;
                 }),
             py::arg("name"),
             py::arg("locations"),
             py::arg("vehicles"),
             py::arg("nodes"),
             py::arg("deliveries"),
             py::arg("driving_seconds") = py::none())
        .def_readonly("name", &PdpRoutePlan::name)
        .def_readonly("locations", &PdpRoutePlan::locations)
        .def_readwrite("vehicles", &PdpRoutePlan::vehicles)
        .def_readonly("nodes", &PdpRoutePlan::nodes)
        .def_readonly("deliveries", &PdpRoutePlan::deliveries)
        .def_readwrite("unassigned_node_indices",
                       &PdpRoutePlan::unassignedNodeIndices)
        .def("rebuild_unassigned_pool",
             &PdpRoutePlan::rebuildUnassignedPool,
             "Recompute unassigned_node_indices from the current routes.")
        .def_readonly("score", &PdpRoutePlan::score)
        .def("driving_time",
             &PdpRoutePlan::drivingTime,
             py::arg("from_location"),
             py::arg("to_location"),
             "Precomputed driving seconds between two location indices.")
        // Convenience: routes as lists of stop ids, per vehicle.
        .def("routes",
             [](const PdpRoutePlan &plan)
             {
                 py::dict out;
                 for (const Vehicle &v : plan.vehicles)
                 {
                     py::list route;
                     for (int i : v.nodeIndices)
                         route.append(
                             plan.nodes[static_cast<std::size_t>(i)].id);
                     out[py::str(v.id)] = route;
                 }
                 return out;
             })
        // Convenience: the ids of the requests left entirely unrouted.
        .def("unassigned_delivery_ids",
             [](const PdpRoutePlan &plan)
             {
                 py::list out;
                 for (const Delivery &d : plan.deliveries)
                 {
                     const Node &pickup = plan.nodes[static_cast<std::size_t>(
                         d.pickupNodeIndex)];
                     if (pickup.vehicleIndex < 0)
                         out.append(d.id);
                 }
                 return out;
             });

    m.def(
        "calculate_score",
        [](PdpRoutePlan &plan)
        { return ScoreCalculator::score(plan); },
        py::arg("plan"),
        "Recompute shadow variables (vehicle_index, arrival_time) and the "
        "HardMediumSoft score (also stores plan.score).");

    m.def("build_random_instance",
          &buildRandomInstance,
          py::arg("request_count"),
          py::arg("vehicle_count"),
          py::arg("seed") = 0,
          "A randomly generated PDPTW instance, for demos and benchmarks.");

    py::enum_<optimaru::AcceptanceRule>(m, "AcceptorType", py::module_local())
        .value("HILL_CLIMBING", optimaru::AcceptanceRule::HillClimbing)
        .value("LATE_ACCEPTANCE", optimaru::AcceptanceRule::LateAcceptance)
        .value("ENTITY_TABU", optimaru::AcceptanceRule::EntityTabu)
        .value("SIMULATED_ANNEALING",
               optimaru::AcceptanceRule::SimulatedAnnealing)
        .value("GREAT_DELUGE", optimaru::AcceptanceRule::GreatDeluge)
        .value("STEP_COUNTING_HILL_CLIMBING",
               optimaru::AcceptanceRule::StepCountingHillClimbing)
        .value("RECORD_TO_RECORD_TRAVEL",
               optimaru::AcceptanceRule::RecordToRecordTravel);

    py::enum_<optimaru::SelectionScheme>(m, "OperatorSelectionType", py::module_local())
        .value("ROULETTE_WHEEL",
               optimaru::SelectionScheme::RouletteWheel)
        .value("SEGMENTED_ROULETTE_WHEEL",
               optimaru::SelectionScheme::SegmentedRouletteWheel)
        .value("ALPHA_UCB", optimaru::SelectionScheme::AlphaUcb)
        .value("RANDOM", optimaru::SelectionScheme::Uniform);

    optimaru::python::bind_config_copy(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.time_limit = optimaru::python::seconds_to_millis(s, "spent_limit_seconds"); })
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.target_score.hard,
                                      c.target_score.medium,
                                      c.target_score.soft);
            },
            [](Solver::Config &c,
               std::tuple<std::int64_t, std::int64_t, std::int64_t> hms)
            {
                c.target_score = Score{
                    std::get<0>(hms), std::get<1>(hms), std::get<2>(hms)};
            })
        .def_readwrite("random_seed", &Solver::Config::seed)
        .def_readwrite("step_count_limit", &Solver::Config::step_limit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::stall_step_limit)
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.stall_time_limit)
                    .count();
            },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "unimproved_spent_limit_seconds"); })
        .def_readwrite("parallel_solver_count",
                       &Solver::Config::parallelSolverCount)
        .def_readwrite("first_fit_decreasing",
                       &Solver::Config::firstFitDecreasing)
        .def_readwrite("pair_construction_heuristic",
                       &Solver::Config::pairConstructionHeuristic)
        // --- acceptance ---
        .def_readwrite("acceptor_type", &Solver::Config::rule)
        .def_readwrite("late_acceptance_size",
                       &Solver::Config::history_length)
        .def_readwrite("record_to_record_start_gap",
                       &Solver::Config::recordToRecordStartGap)
        .def_readwrite("record_to_record_end_gap",
                       &Solver::Config::recordToRecordEndGap)
        // --- move mix ---
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocate_weight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::list_swap_weight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tail_swap_weight)
        .def_readwrite("inter_exchange_move_weight",
                       &Solver::Config::cross_exchange_weight)
        .def_readwrite("inter_exchange_max_segment",
                       &Solver::Config::cross_exchange_max_run)
        // --- pair-aware operators ---
        .def_readwrite("pair_moves", &Solver::Config::pairMoves)
        .def_readwrite("pair_exchange_moves",
                       &Solver::Config::pairExchangeMoves)
        .def_readwrite("pair_exchange_min_nodes",
                       &Solver::Config::pairExchangeMinNodes)
        .def_readwrite("pair_repair_probability",
                       &Solver::Config::pairRepairProbability)
        .def_readwrite("pair_assignment_probability",
                       &Solver::Config::pairAssignmentProbability)
        .def_readwrite("pair_ruin", &Solver::Config::pairRuin)
        // --- adaptive ruin & recreate (ALNS) ---
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger",
                       &Solver::Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction",
                       &Solver::Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction",
                       &Solver::Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count",
                       &Solver::Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
        .def_readwrite("descent_max_sweeps", &Solver::Config::descent_max_sweeps)
        .def_readwrite("route_elimination_weight",
                       &Solver::Config::route_elimination_weight)
        .def_readwrite("route_elimination_max_size",
                       &Solver::Config::route_elimination_max_size)
        .def_readwrite("ruin_recreate_selection_type",
                       &Solver::Config::ruinRecreateSelectionType)
        .def_readwrite("ruin_recreate_operator_decay",
                       &Solver::Config::ruinRecreateOperatorDecay)
        .def_readwrite("ruin_recreate_rewards",
                       &Solver::Config::ruinRecreateRewards)
        .def_readwrite("ruin_recreate_selection_segment_length",
                       &Solver::Config::ruinRecreateSelectionSegmentLength)
        .def_readwrite("ruin_recreate_selection_alpha",
                       &Solver::Config::ruinRecreateSelectionAlpha)
        .def_readwrite("sisr_ruin", &Solver::Config::sisr_ruin)
        .def_readwrite("route_ruin", &Solver::Config::route_ruin)
        .def_readwrite("sisr_max_string_removals",
                       &Solver::Config::sisr_max_string_removals)
        .def_readwrite("sisr_max_string_size",
                       &Solver::Config::sisr_max_string_size)
        .def_readwrite("regret_k", &Solver::Config::regret_k)
        .def_readwrite("blink_rate", &Solver::Config::blink_rate)
        // --- nearby selection ---
        .def_readwrite("driving_time_nearby",
                       &Solver::Config::drivingTimeNearby)
        .def_readwrite("driving_time_nearby_size",
                       &Solver::Config::drivingTimeNearbySize)
        .def_readwrite("driving_time_nearby_probability",
                       &Solver::Config::drivingTimeNearbyProbability)
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        .def(
            "solve",
            [](Solver &solver, PdpRoutePlan &plan)
            {
                solver.solve(plan);
                ScoreCalculator::score(plan);
            },
            py::arg("plan"),
            // Released for the solve: with parallel_solver_count > 1 the
            // workers copy, call and destroy the on_new_best wrapper, each
            // of which takes the GIL back (holding it here deadlocked).
            py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // optimaru.pickup_delivery.constraints). The picks replace the defaults; None
        // keeps them. A name may be picked several times: every pick is its
        // own instance (a fresh copy of the default component) with its own
        // knobs.
        //
        // updateShadowVariables, not score: the plugin solve already
        // stored the score THIS calculator computed, and a full recompute
        // would overwrite it with the monolithic default one.
        //
        // The picks are read with the GIL held; the solve itself releases it
        // like solve() above (an on_new_best callback takes it back).
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               PdpRoutePlan &plan,
               const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); },
                                       constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(plan, calculator);
                ScoreCalculator::updateShadowVariables(plan);
            },
            py::arg("plan"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `plan` — the input
    // optimaru.pickup_delivery.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](PdpRoutePlan &plan, const py::object &constraints)
        {
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, plan);
        },
        py::arg("plan"),
        py::arg("constraints") = py::none(),
        "Raw per-unit cost of every constraint, before weight/exponent/offset/level "
        "(each entry also carries the domain's medium_fold, a per-vehicle one its "
        "evaluated units).");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.pickup_delivery.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only
    // (unassigned_requests_cost: medium, weight 1 in medium units).
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
