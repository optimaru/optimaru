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
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace pdp;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() = "Pickup and delivery (PDPTW) — C++ core exposed to Python";

    opy::bind_score<Score>(m);

    py::class_<Location>(m, "Location", py::module_local())
            .def(py::init([](double latitude, double longitude) {
                     Location l;
                     l.latitude = latitude;
                     l.longitude = longitude;
                     return l;
                 }),
                 py::arg("latitude"), py::arg("longitude"))
            .def_readwrite("latitude", &Location::latitude)
            .def_readwrite("longitude", &Location::longitude)
            .def_readonly("index", &Location::index);

    py::class_<Node>(m, "Node", py::module_local())
            .def(py::init([](std::string id, int locationIndex, Seconds minStart, Seconds maxEnd,
                             Seconds service, int deliveryIndex, bool pickup) {
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
                 py::arg("id"), py::arg("location_index"), py::arg("min_start_time"),
                 py::arg("max_end_time"), py::arg("service_duration"), py::arg("delivery_index"),
                 py::arg("pickup"))
            .def_readwrite("id", &Node::id)
            .def_readwrite("location_index", &Node::locationIndex)
            .def_readwrite("min_start_time", &Node::minStartTime)
            .def_readwrite("max_end_time", &Node::maxEndTime)
            .def_readwrite("service_duration", &Node::serviceDuration)
            .def_readwrite("delivery_index", &Node::deliveryIndex)
            .def_readwrite("pickup", &Node::pickup)
            // Derived fields, recomputed by calculate_score / solve.
            .def_readonly("vehicle_index", &Node::vehicleIndex)
            .def_readonly("arrival_time", &Node::arrivalTime);

    py::class_<Delivery>(m, "Delivery", py::module_local())
            .def(py::init([](std::string id, int pickupNode, int dropoffNode, int demand,
                             Seconds orderTime, int requiredVehicleIndex, int groupIndex) {
                     Delivery d;
                     d.id = std::move(id);
                     d.pickupNodeIndex = pickupNode;
                     d.dropoffNodeIndex = dropoffNode;
                     d.demand = demand;
                     d.orderTime = orderTime;
                     d.requiredVehicleIndex = requiredVehicleIndex;
                     d.groupIndex = groupIndex;
                     return d;
                 }),
                 py::arg("id"), py::arg("pickup_node_index"), py::arg("dropoff_node_index"),
                 py::arg("demand"), py::arg("order_time") = 0,
                 py::arg("required_vehicle_index") = -1, py::arg("group_index") = -1)
            .def_readwrite("id", &Delivery::id)
            .def_readwrite("pickup_node_index", &Delivery::pickupNodeIndex)
            .def_readwrite("dropoff_node_index", &Delivery::dropoffNodeIndex)
            .def_readwrite("demand", &Delivery::demand)
            .def_readwrite("order_time", &Delivery::orderTime)
            .def_readwrite("required_vehicle_index", &Delivery::requiredVehicleIndex)
            .def_readwrite("group_index", &Delivery::groupIndex);

    py::class_<Vehicle>(m, "Vehicle", py::module_local())
            .def(py::init([](std::string id, int capacity, int homeLocationIndex,
                             Seconds departureTime, Seconds maxTravelTime, Seconds maxRouteDuration,
                             Seconds maxEndTime, int maxStops, double minLoadRatio,
                             double fixedCost, double maxRouteRadiusMeters, bool returnToDepot) {
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
                 py::arg("id"), py::arg("capacity"), py::arg("home_location_index"),
                 py::arg("departure_time") = 0, py::arg("max_travel_time") = 0,
                 py::arg("max_route_duration") = 0, py::arg("max_end_time") = 0,
                 py::arg("max_stops") = 0, py::arg("min_load_ratio") = 0.0,
                 py::arg("fixed_cost") = 0.0, py::arg("max_route_radius_meters") = 0.0,
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
            .def_readwrite("max_route_radius_meters", &Vehicle::maxRouteRadiusMeters)
            .def_readwrite("return_to_depot", &Vehicle::returnToDepot,
                           "Whether the route ends back home (default: open route).")
            // The decision: the ordered stop sequence.
            .def_readwrite("node_indices", &Vehicle::nodeIndices)
            .def_readwrite("pinned_node_indices", &Vehicle::pinnedNodeIndices);

    py::class_<PdpRoutePlan>(m, "PdpRoutePlan", py::module_local())
            .def(py::init([](std::string name, std::vector<Location> locations,
                             std::vector<Vehicle> vehicles, std::vector<Node> nodes,
                             std::vector<Delivery> deliveries,
                             std::optional<std::vector<std::vector<Seconds>>> drivingSeconds) {
                     PdpRoutePlan plan;
                     plan.name = std::move(name);
                     plan.locations = std::move(locations);
                     for (std::size_t i = 0; i < plan.locations.size(); ++i)
                         plan.locations[i].index = static_cast<int>(i);
                     plan.vehicles = std::move(vehicles);
                     plan.nodes = std::move(nodes);
                     plan.deliveries = std::move(deliveries);
                     if (drivingSeconds) {
                         const std::size_t n = plan.locations.size();
                         if (drivingSeconds->size() != n)
                             throw std::invalid_argument(
                                     "driving_seconds must be a square matrix over "
                                     "locations");
                         for (const auto& row : *drivingSeconds) {
                             if (row.size() != n)
                                 throw std::invalid_argument(
                                         "driving_seconds must be a square matrix "
                                         "over locations");
                         }
                         plan.drivingSeconds = std::move(*drivingSeconds);
                     } else {
                         HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
                     }
                     return plan;
                 }),
                 py::arg("name"), py::arg("locations"), py::arg("vehicles"), py::arg("nodes"),
                 py::arg("deliveries"), py::arg("driving_seconds") = py::none())
            .def_readonly("name", &PdpRoutePlan::name)
            .def_readonly("locations", &PdpRoutePlan::locations)
            .def_readwrite("vehicles", &PdpRoutePlan::vehicles)
            .def_readonly("nodes", &PdpRoutePlan::nodes)
            .def_readonly("deliveries", &PdpRoutePlan::deliveries)
            .def_readwrite("unassigned_node_indices", &PdpRoutePlan::unassignedNodeIndices)
            .def("rebuild_unassigned_pool", &PdpRoutePlan::rebuildUnassignedPool,
                 "Recompute unassigned_node_indices from the current routes.")
            .def_readonly("score", &PdpRoutePlan::score)
            .def("driving_time", &PdpRoutePlan::drivingTime, py::arg("from_location"),
                 py::arg("to_location"),
                 "Precomputed driving seconds between two location indices.")
            // Convenience: routes as lists of stop ids, per vehicle.
            .def("routes",
                 [](const PdpRoutePlan& plan) {
                     py::dict out;
                     for (const Vehicle& v : plan.vehicles) {
                         py::list route;
                         for (int i : v.nodeIndices)
                             route.append(plan.nodes[static_cast<std::size_t>(i)].id);
                         out[py::str(v.id)] = route;
                     }
                     return out;
                 })
            // Convenience: the ids of the requests left entirely unrouted.
            .def("unassigned_delivery_ids", [](const PdpRoutePlan& plan) {
                py::list out;
                for (const Delivery& d : plan.deliveries) {
                    const Node& pickup = plan.nodes[static_cast<std::size_t>(d.pickupNodeIndex)];
                    if (pickup.vehicleIndex < 0) out.append(d.id);
                }
                return out;
            });

    m.def(
            "calculate_score", [](PdpRoutePlan& plan) { return calculateScore(plan); },
            py::arg("plan"),
            "Recompute the derived fields (vehicle_index, arrival_time) and the "
            "hard / medium / soft score (also stores plan.score). The score is the "
            "one solve(plan) stores: the seven core rules, or the default constraints "
            "when the plan has a group of two or more requests.");

    m.def("build_random_instance", &buildRandomInstance, py::arg("request_count"),
          py::arg("vehicle_count"), py::arg("seed") = 0,
          "A randomly generated PDPTW instance, for demos and benchmarks.");

    opy::bind_acceptor_type(m);

    py::enum_<optimaru::SelectionScheme>(m, "OperatorSelectionType", py::module_local())
            .value("ROULETTE_WHEEL", optimaru::SelectionScheme::RouletteWheel)
            .value("SEGMENTED_ROULETTE_WHEEL", optimaru::SelectionScheme::SegmentedRouletteWheel)
            .value("ALPHA_UCB", optimaru::SelectionScheme::AlphaUcb)
            .value("RANDOM", optimaru::SelectionScheme::Uniform);

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_property(
                    "best_score_limit",
                    [](const Solver::Config& c) {
                        return py::make_tuple(c.target_score.hard, c.target_score.medium,
                                              c.target_score.soft);
                    },
                    [](Solver::Config& c,
                       std::tuple<std::int64_t, std::int64_t, std::int64_t> hms) {
                        c.target_score =
                                Score{std::get<0>(hms), std::get<1>(hms), std::get<2>(hms)};
                    })
            .def_readwrite("first_fit_decreasing", &Solver::Config::first_fit_decreasing)
            .def_readwrite("pair_construction_heuristic",
                           &Solver::Config::pair_construction_heuristic)
            // --- move mix ---
            .def_readwrite("relocate_move_weight", &Solver::Config::relocate_weight)
            .def_readwrite("list_swap_move_weight", &Solver::Config::list_swap_weight)
            .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
            .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
            .def_readwrite("tail_swap_move_weight", &Solver::Config::tail_swap_weight)
            .def_readwrite("inter_exchange_move_weight", &Solver::Config::cross_exchange_weight)
            .def_readwrite("inter_exchange_max_segment", &Solver::Config::cross_exchange_max_run)
            // --- pair operators ---
            .def_readwrite("pair_moves", &Solver::Config::pair_moves)
            .def_readwrite("pair_exchange_moves", &Solver::Config::pair_exchange_moves)
            .def_readwrite("pair_exchange_min_nodes", &Solver::Config::pair_exchange_min_nodes)
            .def_readwrite("pair_repair_probability", &Solver::Config::pair_repair_probability)
            .def_readwrite("pair_assignment_probability",
                           &Solver::Config::pair_assignment_probability)
            // --- request-level moves ---
            .def_readwrite("request_ruin_recreate", &Solver::Config::request_ruin_recreate)
            .def_property(
                    "request_ruin_max_requests",
                    [](const Solver::Config& c) { return c.request_ruin_max_requests; },
                    [](Solver::Config& c, int count) {
                        if (count <= 0) {
                            throw py::value_error("request_ruin_max_requests must be positive");
                        }
                        c.request_ruin_max_requests = count;
                    })
            .def_readwrite("bundle_moves", &Solver::Config::bundle_moves)
            .def_property(
                    "bundle_max_requests",
                    [](const Solver::Config& c) { return c.bundle_max_requests; },
                    [](Solver::Config& c, int count) {
                        if (count <= 0) {
                            throw py::value_error("bundle_max_requests must be positive");
                        }
                        c.bundle_max_requests = count;
                    })
            .def_readwrite("pair_ruin", &Solver::Config::pair_ruin)
            // --- adaptive ruin & recreate (ALNS) ---
            // (bind_solver_config binds the shared ruin_recreate* knobs.)
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            .def_readwrite("descent_max_sweeps", &Solver::Config::descent_max_sweeps)
            .def_readwrite("route_elimination_weight", &Solver::Config::route_elimination_weight)
            .def_readwrite("route_elimination_max_size",
                           &Solver::Config::route_elimination_max_size)
            .def_readwrite("ruin_recreate_selection_type",
                           &Solver::Config::ruin_recreate_selection_type)
            .def_readwrite("ruin_recreate_selection_segment_length",
                           &Solver::Config::ruin_recreate_selection_segment_length)
            .def_readwrite("ruin_recreate_selection_alpha",
                           &Solver::Config::ruin_recreate_selection_alpha)
            .def_readwrite("sisr_ruin", &Solver::Config::sisr_ruin)
            .def_readwrite("route_ruin", &Solver::Config::route_ruin)
            .def_readwrite("sisr_max_string_removals", &Solver::Config::sisr_max_string_removals)
            .def_readwrite("sisr_max_string_size", &Solver::Config::sisr_max_string_size)
            .def_readwrite("regret_k", &Solver::Config::regret_k)
            .def_readwrite("blink_rate", &Solver::Config::blink_rate)
            // --- nearby selection ---
            .def_readwrite("driving_time_nearby", &Solver::Config::driving_time_nearby)
            .def_readwrite("driving_time_nearby_size", &Solver::Config::driving_time_nearby_size)
            .def_readwrite("driving_time_nearby_probability",
                           &Solver::Config::driving_time_nearby_probability);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            .def(
                    "solve",
                    [](Solver& solver, PdpRoutePlan& plan) {
                        solver.solve(plan);
                        // A plan with groups was scored with the default
                        // constraints (Solver::solve), derived fields
                        // included; the core score would leave the group rule
                        // out of plan.score.
                        if (!plan.hasRequestGroups()) ScoreCalculator::score(plan);
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
            // Every parallel start gets a calculator of its own, built from the
            // same picks. The picks are Python objects, read with the GIL held:
            // the first calculator is built before the solve releases the GIL
            // (so a bad pick raises with nothing started), the others by the
            // factory, which takes the GIL back for the read. The solve itself
            // runs without it, like solve() above (an on_new_best callback
            // takes it back).
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, PdpRoutePlan& plan, const py::object& constraints) {
                        const auto build = [&constraints] {
                            auto calculator = std::make_unique<DecomposedCalculator>();
                            opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                    .into(*calculator);
                            return calculator;
                        };
                        std::unique_ptr<DecomposedCalculator> first = build();
                        py::gil_scoped_release release;
                        solver.solve(plan, [&first, &build]() {
                            if (first) return std::move(first);
                            py::gil_scoped_acquire gil;
                            return build();
                        });
                        ScoreCalculator::updateShadowVariables(plan);
                    },
                    py::arg("plan"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly. The units of constraint_costs() are the vehicles; a
    // per-vehicle constraint lists the ones it evaluates as "units".
    opy::bind_constraints<PdpRoutePlan>(m, "plan", [] { return defaultConstraints(); });
}
