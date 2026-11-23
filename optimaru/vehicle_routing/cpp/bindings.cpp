// pybind11 bindings for the vehicle-routing C++ core.
// Wraps the existing vrp::* use case (vrp/ beside this file) without modifying
// it. One solver, one module (optimaru.vehicle_routing._core).
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"
#include "vrp/constraints.hpp"
#include "vrp/model/domain.hpp"
#include "vrp/model/haversine.hpp"
#include "vrp/score/score_calculator.hpp"
#include "vrp/solver.hpp"

namespace py = pybind11;
using namespace vrp;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() = "Vehicle routing — C++ core exposed to Python";

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

    py::class_<Visit>(m, "Visit", py::module_local())
            .def(py::init([](std::string id, std::string name, int locationIndex, int demand,
                             Seconds minStart, Seconds maxEnd, Seconds service,
                             std::int64_t unassignedPenalty, int groupIndex,
                             std::vector<int> afterVisitIndices) {
                     Visit v;
                     v.id = std::move(id);
                     v.name = std::move(name);
                     v.locationIndex = locationIndex;
                     v.demand = demand;
                     v.minStartTime = minStart;
                     v.maxEndTime = maxEnd;
                     v.serviceDuration = service;
                     v.unassignedPenaltyMinutes = unassignedPenalty;
                     v.groupIndex = groupIndex;
                     v.afterVisitIndices = std::move(afterVisitIndices);
                     return v;
                 }),
                 py::arg("id"), py::arg("name"), py::arg("location_index"), py::arg("demand"),
                 py::arg("min_start_time"), py::arg("max_end_time"), py::arg("service_duration"),
                 py::arg("unassigned_penalty_minutes") = Visit::kUnassignedPenaltyAuto,
                 py::arg("group_index") = -1, py::arg("after_visit_indices") = std::vector<int>{})
            .def_readwrite("id", &Visit::id)
            .def_readwrite("name", &Visit::name)
            .def_readwrite("location_index", &Visit::locationIndex)
            .def_readwrite("demand", &Visit::demand)
            .def_readwrite("min_start_time", &Visit::minStartTime)
            .def_readwrite("max_end_time", &Visit::maxEndTime)
            .def_readwrite("service_duration", &Visit::serviceDuration)
            .def_readwrite("unassigned_penalty_minutes", &Visit::unassignedPenaltyMinutes)
            .def_readwrite("group_index", &Visit::groupIndex)
            .def_readwrite("after_visit_indices", &Visit::afterVisitIndices)
            .def("unassigned_penalty", &Visit::unassignedPenalty,
                 "The medium-level cost charged when this visit is left unrouted.")
            // Derived fields, recomputed by calculate_score / solve.
            .def_readonly("vehicle_index", &Visit::vehicleIndex)
            .def_readonly("arrival_time", &Visit::arrivalTime);

    py::class_<Vehicle>(m, "Vehicle", py::module_local())
            .def(py::init([](std::string id, int capacity, int startLocationIndex,
                             Seconds departureTime, Seconds maxTravelTime, Seconds maxRouteDuration,
                             Seconds maxEndTime, int maxStops, double minLoadRatio,
                             double fixedCost, double maxRouteRadiusMeters, int endLocationIndex,
                             int profileIndex) {
                     Vehicle v;
                     v.id = std::move(id);
                     v.capacity = capacity;
                     v.startLocationIndex = startLocationIndex;
                     v.endLocationIndex = endLocationIndex;
                     v.profileIndex = profileIndex;
                     v.departureTime = departureTime;
                     v.maxTravelTime = maxTravelTime;
                     v.maxRouteDuration = maxRouteDuration;
                     v.maxEndTime = maxEndTime;
                     v.maxStops = maxStops;
                     v.minLoadRatio = minLoadRatio;
                     v.fixedCost = fixedCost;
                     v.maxRouteRadiusMeters = maxRouteRadiusMeters;
                     return v;
                 }),
                 py::arg("id"), py::arg("capacity"), py::arg("start_location_index"),
                 py::arg("departure_time") = 0, py::arg("max_travel_time") = 0,
                 py::arg("max_route_duration") = 0, py::arg("max_end_time") = 0,
                 py::arg("max_stops") = 0, py::arg("min_load_ratio") = 0.0,
                 py::arg("fixed_cost") = 0.0, py::arg("max_route_radius_meters") = 0.0,
                 py::arg("end_location_index") = -1, py::arg("profile_index") = 0)
            .def_readwrite("id", &Vehicle::id)
            .def_readwrite("capacity", &Vehicle::capacity)
            .def_readwrite("start_location_index", &Vehicle::startLocationIndex)
            .def_readwrite("end_location_index", &Vehicle::endLocationIndex,
                           "Where the route ends; -1 for an open route (the default), which "
                           "ends at its last visit.")
            .def_readwrite("profile_index", &Vehicle::profileIndex,
                           "Which of the plan's matrices the vehicle drives on.")
            .def_readwrite("departure_time", &Vehicle::departureTime)
            .def_readwrite("max_travel_time", &Vehicle::maxTravelTime)
            .def_readwrite("max_route_duration", &Vehicle::maxRouteDuration)
            .def_readwrite("max_end_time", &Vehicle::maxEndTime)
            .def_readwrite("max_stops", &Vehicle::maxStops)
            .def_readwrite("min_load_ratio", &Vehicle::minLoadRatio)
            .def_readwrite("fixed_cost", &Vehicle::fixedCost)
            .def_readwrite("max_route_radius_meters", &Vehicle::maxRouteRadiusMeters)
            // The decision: the ordered visit indices of the route.
            .def_readwrite("visit_indices", &Vehicle::visitIndices);

    using OptionalMatrices = std::optional<std::vector<std::optional<Matrix>>>;
    py::class_<VehicleRoutePlan>(m, "VehicleRoutePlan", py::module_local())
            .def(py::init([](std::string name, std::vector<Location> locations,
                             std::vector<Vehicle> vehicles, std::vector<Visit> visits,
                             OptionalMatrices durations, OptionalMatrices distances) {
                     VehicleRoutePlan plan;
                     plan.name = std::move(name);
                     plan.locations = std::move(locations);
                     for (std::size_t i = 0; i < plan.locations.size(); ++i)
                         plan.locations[i].index = static_cast<int>(i);
                     plan.vehicles = std::move(vehicles);
                     plan.visits = std::move(visits);
                     // Explicit matrices, one per profile: what production
                     // actually has (a road-network router, a benchmark
                     // instance's own metric). A profile left out or None is
                     // filled from the coordinates (haversine).
                     const std::size_t n = plan.locations.size();
                     auto take = [n](OptionalMatrices& given, std::vector<Matrix>& into,
                                     const char* what) {
                         if (!given) return;
                         for (std::optional<Matrix>& matrix : *given) {
                             if (matrix) {
                                 bool square = matrix->size() == n;
                                 for (const auto& row : *matrix) square = square && row.size() == n;
                                 if (!square)
                                     throw std::invalid_argument(
                                             std::string(what) +
                                             " must be square matrices over the locations");
                                 into.push_back(std::move(*matrix));
                             } else {
                                 into.emplace_back();
                             }
                         }
                     };
                     take(durations, plan.durationMatrices, "durations");
                     take(distances, plan.distanceMatrices, "distances");
                     HaversineDrivingTimeCalculator::fillMissingMatrices(plan);
                     const int profiles = static_cast<int>(plan.durationMatrices.size());
                     const int locationCount = static_cast<int>(n);
                     for (const Vehicle& v : plan.vehicles) {
                         if (v.profileIndex < 0 || v.profileIndex >= profiles)
                             throw std::invalid_argument("vehicle " + v.id + ": profile_index " +
                                                         std::to_string(v.profileIndex) +
                                                         " is not one of the " +
                                                         std::to_string(profiles) + " profiles");
                         if (v.startLocationIndex < 0 || v.startLocationIndex >= locationCount ||
                             v.endLocationIndex < -1 || v.endLocationIndex >= locationCount)
                             throw std::invalid_argument(
                                     "vehicle " + v.id +
                                     ": a depot location index is out of range");
                     }
                     return plan;
                 }),
                 py::arg("name"), py::arg("locations"), py::arg("vehicles"), py::arg("visits"),
                 py::arg("durations") = py::none(), py::arg("distances") = py::none(),
                 "Profiles: as many as the longer of durations / distances lists (one at "
                 "least); a matrix not given is filled from the coordinates.")
            .def_readonly("name", &VehicleRoutePlan::name)
            .def_readonly("locations", &VehicleRoutePlan::locations)
            .def_readwrite("vehicles", &VehicleRoutePlan::vehicles)
            .def_readonly("visits", &VehicleRoutePlan::visits)
            // The unassigned pool: visits the solver chose not to route (the
            // medium-level unassignedVisitsCost penalty). Filled by solve().
            .def_readwrite("unassigned_visit_indices", &VehicleRoutePlan::unassignedVisitIndices)
            .def("rebuild_unassigned_pool", &VehicleRoutePlan::rebuildUnassignedPool,
                 "Recompute unassigned_visit_indices from the current routes.")
            .def_readonly("score", &VehicleRoutePlan::score)
            .def_property_readonly(
                    "profile_count",
                    [](const VehicleRoutePlan& plan) { return plan.durationMatrices.size(); })
            .def(
                    "driving_time",
                    [](const VehicleRoutePlan& plan, int from, int to, int profile) {
                        return plan.durationMatrices.at(static_cast<std::size_t>(profile))
                                .at(static_cast<std::size_t>(from))
                                .at(static_cast<std::size_t>(to));
                    },
                    py::arg("from_location"), py::arg("to_location"), py::arg("profile") = 0,
                    "Driving seconds between two location indices in a profile.")
            .def(
                    "driving_distance",
                    [](const VehicleRoutePlan& plan, int from, int to, int profile) {
                        return plan.distanceMatrices.at(static_cast<std::size_t>(profile))
                                .at(static_cast<std::size_t>(from))
                                .at(static_cast<std::size_t>(to));
                    },
                    py::arg("from_location"), py::arg("to_location"), py::arg("profile") = 0,
                    "Driving meters between two location indices in a profile.")
            // Convenience: routes as lists of visit ids, per vehicle.
            .def("routes",
                 [](const VehicleRoutePlan& plan) {
                     py::dict out;
                     for (const Vehicle& v : plan.vehicles) {
                         py::list route;
                         for (int i : v.visitIndices) route.append(plan.visits[i].id);
                         out[py::str(v.id)] = route;
                     }
                     return out;
                 })
            // Convenience: the ids of the visits left unassigned.
            .def("unassigned_visit_ids", [](const VehicleRoutePlan& plan) {
                py::list out;
                for (int i : plan.unassignedVisitIndices)
                    out.append(plan.visits[static_cast<std::size_t>(i)].id);
                return out;
            });

    m.def(
            "calculate_score", [](VehicleRoutePlan& plan) { return calculateScore(plan); },
            py::arg("plan"),
            "Recompute the derived fields (vehicle_index, arrival_time) and the "
            "hard / medium / soft score (also stores plan.score). The score is the "
            "one solve(plan) stores: the four core constraints, or the default constraints "
            "when the plan has a group of two or more visits.");

    // The acceptance criteria the engine offers.
    opy::bind_acceptor_type(m);

    // The adaptive operator-selection schemes.
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
            // --- move mix ---
            .def_readwrite("relocate_move_weight", &Solver::Config::relocate_weight)
            .def_readwrite("list_swap_move_weight", &Solver::Config::list_swap_weight)
            .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
            .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
            .def_readwrite("tail_swap_move_weight", &Solver::Config::tail_swap_weight)
            .def_readwrite("inter_exchange_move_weight", &Solver::Config::cross_exchange_weight)
            .def_readwrite("inter_exchange_max_segment", &Solver::Config::cross_exchange_max_run)
            // --- adaptive ruin & recreate (ALNS) ---
            // (bind_solver_config binds the shared ruin_recreate* knobs.)
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            .def_readwrite("descent_max_sweeps", &Solver::Config::descent_max_sweeps)
            .def_readwrite("granular_descent", &Solver::Config::granular_descent)
            .def_readwrite("granular_descent_max_segment",
                           &Solver::Config::granular_descent_max_segment)
            .def_readwrite("granular_descent_candidate_routes",
                           &Solver::Config::granular_descent_candidate_routes)
            .def_readwrite("route_elimination_weight", &Solver::Config::route_elimination_weight)
            .def_readwrite("route_elimination_max_size",
                           &Solver::Config::route_elimination_max_size)
            .def_readwrite("ejection_chain_weight", &Solver::Config::ejection_chain_weight)
            .def_readwrite("ejection_chain_max_steps", &Solver::Config::ejection_chain_max_steps)
            .def_readwrite("ejection_chain_candidate_lists",
                           &Solver::Config::ejection_chain_candidate_lists)
            .def_readwrite("ejection_chain_max_evaluations",
                           &Solver::Config::ejection_chain_max_evaluations)
            .def_readwrite("iterated_local_search_weight",
                           &Solver::Config::iterated_local_search_weight)
            .def_readwrite("iterated_local_search_min_ruin",
                           &Solver::Config::iterated_local_search_min_ruin)
            .def_readwrite("iterated_local_search_max_ruin",
                           &Solver::Config::iterated_local_search_max_ruin)
            .def_readwrite("iterated_local_search_max_segment",
                           &Solver::Config::iterated_local_search_max_segment)
            .def_readwrite("iterated_local_search_max_evaluations",
                           &Solver::Config::iterated_local_search_max_evaluations)
            // --- visit groups ---
            .def_readwrite("group_ruin_recreate", &Solver::Config::group_ruin_recreate)
            .def_property(
                    "group_ruin_max_visits",
                    [](const Solver::Config& c) { return c.group_ruin_max_visits; },
                    [](Solver::Config& c, int count) {
                        if (count < 0) {
                            throw py::value_error("group_ruin_max_visits must not be negative");
                        }
                        c.group_ruin_max_visits = count;
                    })
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
            // Solves in place: fills vehicle.visit_indices, the visits' derived
            // fields and plan.score.
            .def(
                    "solve",
                    [](Solver& solver, VehicleRoutePlan& plan) {
                        solver.solve(plan);
                        // A plan with groups was scored with the default
                        // constraints (Solver::solve); the core score would
                        // leave the two group constraints out of plan.score.
                        if (plan.hasVisitGroups()) {
                            ScoreCalculator::updateShadowVariables(plan);
                        } else {
                            ScoreCalculator::score(plan);
                        }
                    },
                    py::arg("plan"),
                    // Released for the solve: with parallel_solver_count > 1 the
                    // workers copy, call and destroy the on_new_best wrapper, each
                    // of which takes the GIL back (holding it here deadlocked).
                    py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.vehicle_routing.constraints). The picks replace the defaults; None
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
            // factory, which takes the GIL back for the read. A calculator
            // holds the values read from the picks and no Python reference, so
            // it is used and destroyed without the GIL. The solve itself runs
            // without it, like solve() above (an on_new_best callback takes
            // it back).
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, VehicleRoutePlan& plan, const py::object& constraints) {
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
    // default assembly. The units of constraint_costs() are the vehicles; a per-vehicle
    // constraint lists the ones it evaluates as "units". The plan is checked
    // first, as before a solve: its after_visit_indices are written from
    // Python and visitOrderCost reads them as indices.
    opy::bind_constraints<VehicleRoutePlan>(
            m, "plan", [] { return defaultConstraints(); },
            [](VehicleRoutePlan& plan) { checkVisitGroups(plan); });
}
