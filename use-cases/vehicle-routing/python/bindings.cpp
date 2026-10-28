// pybind11 bindings for the vehicle-routing C++ core.
// Wraps the existing vrp::* use case (../cpp) without modifying
// it. One solver, one module (vrp_routing._core).
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <tuple>

#include "optimaru/python/constraint_binding.hpp"
#include "vrp/constraints.hpp"
#include "vrp/domain.hpp"
#include "vrp/haversine.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/solver.hpp"

namespace py = pybind11;
using namespace vrp;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Vehicle routing — C++ core exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("medium", &Score::medium)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

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

    py::class_<Visit>(m, "Visit", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    int locationIndex,
                    int demand,
                    Seconds minStart,
                    Seconds maxEnd,
                    Seconds service,
                    std::int64_t unassignedPenalty)
                 {
                     Visit v;
                     v.id = std::move(id);
                     v.name = std::move(name);
                     v.locationIndex = locationIndex;
                     v.demand = demand;
                     v.minStartTime = minStart;
                     v.maxEndTime = maxEnd;
                     v.serviceDuration = service;
                     v.unassignedPenaltyMinutes = unassignedPenalty;
                     return v;
                 }),
             py::arg("id"),
             py::arg("name"),
             py::arg("location_index"),
             py::arg("demand"),
             py::arg("min_start_time"),
             py::arg("max_end_time"),
             py::arg("service_duration"),
             py::arg("unassigned_penalty_minutes")
             = Visit::kUnassignedPenaltyAuto)
        .def_readwrite("id", &Visit::id)
        .def_readwrite("name", &Visit::name)
        .def_readwrite("location_index", &Visit::locationIndex)
        .def_readwrite("demand", &Visit::demand)
        .def_readwrite("min_start_time", &Visit::minStartTime)
        .def_readwrite("max_end_time", &Visit::maxEndTime)
        .def_readwrite("service_duration", &Visit::serviceDuration)
        .def_readwrite("unassigned_penalty_minutes",
                       &Visit::unassignedPenaltyMinutes)
        .def("unassigned_penalty",
             &Visit::unassignedPenalty,
             "The medium-level cost charged when this visit is left unrouted.")
        // Shadow variables, recomputed by calculate_score / solve.
        .def_readonly("vehicle_index", &Visit::vehicleIndex)
        .def_readonly("arrival_time", &Visit::arrivalTime);

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
        // The planning list variable: the ordered visit indices of the route.
        .def_readwrite("visit_indices", &Vehicle::visitIndices);

    py::class_<VehicleRoutePlan>(m, "VehicleRoutePlan", py::module_local())
        .def(py::init(
                 [](std::string name,
                    std::vector<Location> locations,
                    std::vector<Vehicle> vehicles,
                    std::vector<Visit> visits,
                    std::optional<std::vector<std::vector<Seconds>>>
                        drivingSeconds)
                 {
                     VehicleRoutePlan plan;
                     plan.name = std::move(name);
                     plan.locations = std::move(locations);
                     for (std::size_t i = 0; i < plan.locations.size(); ++i)
                         plan.locations[i].index = static_cast<int>(i);
                     plan.vehicles = std::move(vehicles);
                     plan.visits = std::move(visits);
                     if (drivingSeconds)
                     {
                         // An explicit matrix: what production actually has
                         // (OSRM / Google / a benchmark instance's own
                         // metric). Haversine is only the fallback for when
                         // the caller has nothing but coordinates.
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
                         // The haversine driving-time matrix, precomputed once
                         // (the Java Location map analog).
                         HaversineDrivingTimeCalculator::initDrivingTimeMatrix(
                             plan);
                     }
                     return plan;
                 }),
             py::arg("name"),
             py::arg("locations"),
             py::arg("vehicles"),
             py::arg("visits"),
             py::arg("driving_seconds") = py::none())
        .def_readonly("name", &VehicleRoutePlan::name)
        .def_readonly("locations", &VehicleRoutePlan::locations)
        .def_readwrite("vehicles", &VehicleRoutePlan::vehicles)
        .def_readonly("visits", &VehicleRoutePlan::visits)
        // The unassigned pool: visits the solver chose not to route (the
        // medium-level maximizeVisitsAssigned penalty). Filled by solve().
        .def_readwrite("unassigned_visit_indices",
                       &VehicleRoutePlan::unassignedVisitIndices)
        .def("rebuild_unassigned_pool",
             &VehicleRoutePlan::rebuildUnassignedPool,
             "Recompute unassigned_visit_indices from the current routes.")
        .def_readonly("score", &VehicleRoutePlan::score)
        .def("driving_time",
             &VehicleRoutePlan::drivingTime,
             py::arg("from_location"),
             py::arg("to_location"),
             "Precomputed driving seconds between two location indices.")
        // Convenience: routes as lists of visit ids, per vehicle.
        .def("routes",
             [](const VehicleRoutePlan &plan)
             {
                 py::dict out;
                 for (const Vehicle &v : plan.vehicles)
                 {
                     py::list route;
                     for (int i : v.visitIndices)
                         route.append(plan.visits[i].id);
                     out[py::str(v.id)] = route;
                 }
                 return out;
             })
        // Convenience: the ids of the visits left unassigned.
        .def("unassigned_visit_ids",
             [](const VehicleRoutePlan &plan)
             {
                 py::list out;
                 for (int i : plan.unassignedVisitIndices)
                     out.append(plan.visits[static_cast<std::size_t>(i)].id);
                 return out;
             });

    m.def(
        "calculate_score",
        [](VehicleRoutePlan &plan)
        { return ScoreCalculator::calculateScore(plan); },
        py::arg("plan"),
        "Recompute shadow variables (vehicle_index, arrival_time) and the "
        "HardMediumSoft score (also stores plan.score).");

    // The acceptance criteria the engine offers.
    py::enum_<optimaru::AcceptorType>(m, "AcceptorType", py::module_local())
        .value("HILL_CLIMBING", optimaru::AcceptorType::HILL_CLIMBING)
        .value("LATE_ACCEPTANCE", optimaru::AcceptorType::LATE_ACCEPTANCE)
        .value("ENTITY_TABU", optimaru::AcceptorType::ENTITY_TABU)
        .value("SIMULATED_ANNEALING",
               optimaru::AcceptorType::SIMULATED_ANNEALING)
        .value("GREAT_DELUGE", optimaru::AcceptorType::GREAT_DELUGE)
        .value("STEP_COUNTING_HILL_CLIMBING",
               optimaru::AcceptorType::STEP_COUNTING_HILL_CLIMBING)
        .value("RECORD_TO_RECORD_TRAVEL",
               optimaru::AcceptorType::RECORD_TO_RECORD_TRAVEL);

    // The adaptive operator-selection schemes.
    py::enum_<optimaru::OperatorSelectionType>(m, "OperatorSelectionType", py::module_local())
        .value("ROULETTE_WHEEL",
               optimaru::OperatorSelectionType::ROULETTE_WHEEL)
        .value("SEGMENTED_ROULETTE_WHEEL",
               optimaru::OperatorSelectionType::SEGMENTED_ROULETTE_WHEEL)
        .value("ALPHA_UCB", optimaru::OperatorSelectionType::ALPHA_UCB)
        .value("RANDOM", optimaru::OperatorSelectionType::RANDOM);

    py::class_<Solver::Config>(m, "SolverConfig", py::module_local())
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Solver::Config &c, double s)
            {
                c.spentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
        .def_readwrite("late_acceptance_size",
                       &Solver::Config::lateAcceptanceSize)
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.medium,
                                      c.bestScoreLimit.soft);
            },
            [](Solver::Config &c,
               std::tuple<std::int64_t, std::int64_t, std::int64_t> hms)
            {
                c.bestScoreLimit = Score{
                    std::get<0>(hms), std::get<1>(hms), std::get<2>(hms)};
            })
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::unimprovedStepCountLimit)
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Solver::Config &c, double s)
            {
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("parallel_solver_count",
                       &Solver::Config::parallelSolverCount)
        .def_readwrite("first_fit_decreasing",
                       &Solver::Config::firstFitDecreasing)
        // --- acceptance ---
        .def_readwrite("acceptor_type", &Solver::Config::acceptorType)
        .def_readwrite("record_to_record_start_gap",
                       &Solver::Config::recordToRecordStartGap)
        .def_readwrite("record_to_record_end_gap",
                       &Solver::Config::recordToRecordEndGap)
        // --- move mix ---
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocateMoveWeight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::listSwapMoveWeight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::twoOptMoveWeight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::orOptMoveWeight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tailSwapMoveWeight)
        .def_readwrite("inter_exchange_move_weight",
                       &Solver::Config::interExchangeMoveWeight)
        .def_readwrite("inter_exchange_max_segment",
                       &Solver::Config::interExchangeMaxSegment)
        // --- adaptive ruin & recreate (ALNS) ---
        .def_readwrite("ruin_recreate", &Solver::Config::ruinRecreate)
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
        .def_readwrite("descent_weight", &Solver::Config::descentWeight)
        .def_readwrite("descent_max_sweeps", &Solver::Config::descentMaxSweeps)
        .def_readwrite("granular_descent", &Solver::Config::granularDescent)
        .def_readwrite("granular_descent_max_segment",
                       &Solver::Config::granularDescentMaxSegment)
        .def_readwrite("granular_descent_candidate_routes",
                       &Solver::Config::granularDescentCandidateRoutes)
        .def_readwrite("route_elimination_weight",
                       &Solver::Config::routeEliminationWeight)
        .def_readwrite("route_elimination_max_size",
                       &Solver::Config::routeEliminationMaxSize)
        .def_readwrite("ejection_chain_weight",
                       &Solver::Config::ejectionChainWeight)
        .def_readwrite("ejection_chain_max_steps",
                       &Solver::Config::ejectionChainMaxSteps)
        .def_readwrite("ejection_chain_candidate_lists",
                       &Solver::Config::ejectionChainCandidateLists)
        .def_readwrite("ejection_chain_max_evaluations",
                       &Solver::Config::ejectionChainMaxEvaluations)
        .def_readwrite("iterated_local_search_weight",
                       &Solver::Config::iteratedLocalSearchWeight)
        .def_readwrite("iterated_local_search_min_ruin",
                       &Solver::Config::iteratedLocalSearchMinRuin)
        .def_readwrite("iterated_local_search_max_ruin",
                       &Solver::Config::iteratedLocalSearchMaxRuin)
        .def_readwrite("iterated_local_search_max_segment",
                       &Solver::Config::iteratedLocalSearchMaxSegment)
        .def_readwrite("iterated_local_search_max_evaluations",
                       &Solver::Config::iteratedLocalSearchMaxEvaluations)
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
        .def_readwrite("sisr_ruin", &Solver::Config::sisrRuin)
        .def_readwrite("route_ruin", &Solver::Config::routeRuin)
        .def_readwrite("sisr_max_string_removals",
                       &Solver::Config::sisrMaxStringRemovals)
        .def_readwrite("sisr_max_string_size",
                       &Solver::Config::sisrMaxStringSize)
        .def_readwrite("regret_k", &Solver::Config::regretK)
        .def_readwrite("blink_rate", &Solver::Config::blinkRate)
        // --- nearby selection ---
        .def_readwrite("driving_time_nearby",
                       &Solver::Config::drivingTimeNearby)
        .def_readwrite("driving_time_nearby_size",
                       &Solver::Config::drivingTimeNearbySize)
        .def_readwrite("driving_time_nearby_probability",
                       &Solver::Config::drivingTimeNearbyProbability)
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: fills vehicle.visit_indices, the visits' shadow
        // variables and plan.score.
        .def(
            "solve",
            [](Solver &solver, VehicleRoutePlan &plan)
            {
                solver.solve(plan);
                ScoreCalculator::calculateScore(plan);
            },
            py::arg("plan"))
        // Same solve, scored by a caller-chosen constraint set (see
        // vrp_routing.constraints). The picks replace the defaults; None
        // keeps them.
        //
        // updateShadowVariables, not calculateScore: the plugin solve already
        // stored the score THIS calculator computed, and a full recompute
        // would overwrite it with the monolithic default one.
        //
        // The GIL is held for the duration, matching this package's plain
        // solve() above — neither releases it, so a long solve blocks other
        // Python threads here. Deliberate: the caller-owned calculator is not
        // cloned across workers on this path, so there is no worker thread
        // that would need the GIL back for an on_new_best callback.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               VehicleRoutePlan &plan,
               const py::object &constraints)
            {
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints)
                    .into(calculator);
                solver.solve(plan, calculator);
                ScoreCalculator::updateShadowVariables(plan);
            },
            py::arg("plan"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `plan` — the input
    // vrp_routing.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](VehicleRoutePlan &plan)
        {
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            return tfpy::constraintCosts(calculator, plan);
        },
        py::arg("plan"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order --
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraintNames(defaultConstraints()); },
        "Constraint names available to vrp_routing.constraints.");
}
