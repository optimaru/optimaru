// pybind11 bindings for the facility-location C++ domain wrappers.
// The Python extension links the existing fl::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fl/constraints.hpp"
#include "fl/domain.hpp"
#include "fl/score_calculator.hpp"
#include "fl/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace fl;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Facility location — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Location>(m, "Location", py::module_local())
        .def(py::init(
                 [](double latitude, double longitude)
                 {
                     Location location;
                     location.latitude = latitude;
                     location.longitude = longitude;
                     return location;
                 }),
             py::arg("latitude"),
             py::arg("longitude"))
        .def_readwrite("latitude", &Location::latitude)
        .def_readwrite("longitude", &Location::longitude)
        // Java Location.getDistanceTo: ceil(Euclidean degrees * 111 km).
        .def("distance_to", &Location::distanceTo, py::arg("other"));

    py::class_<Facility>(m, "Facility", py::module_local())
        .def(py::init(
                 [](std::string id,
                    Location location,
                    std::int64_t setupCost,
                    std::int64_t capacity)
                 {
                     Facility facility;
                     facility.id = std::move(id);
                     facility.location = location;
                     facility.setupCost = setupCost;
                     facility.capacity = capacity;
                     return facility;
                 }),
             py::arg("id"),
             py::arg("location"),
             py::arg("setup_cost"),
             py::arg("capacity"))
        .def_readwrite("id", &Facility::id)
        .def_readwrite("location", &Facility::location)
        .def_readwrite("setup_cost", &Facility::setupCost)
        .def_readwrite("capacity", &Facility::capacity);

    py::class_<Consumer>(m, "Consumer", py::module_local())
        .def(py::init(
                 [](std::string id, Location location, std::int64_t demand)
                 {
                     Consumer consumer;
                     consumer.id = std::move(id);
                     consumer.location = location;
                     consumer.demand = demand;
                     return consumer;
                 }),
             py::arg("id"),
             py::arg("location"),
             py::arg("demand"))
        .def_readwrite("id", &Consumer::id)
        .def_readwrite("location", &Consumer::location)
        .def_readwrite("demand", &Consumer::demand)
        // The planning variable: index into problem.facilities, -1 ==
        // unassigned (Java null).
        .def_readwrite("facility", &Consumer::facility)
        .def("is_assigned", &Consumer::isAssigned);

    py::class_<FacilityLocationProblem>(m, "FacilityLocationProblem", py::module_local())
        .def(py::init(
                 [](std::vector<Facility> facilities,
                    std::vector<Consumer> consumers,
                    Location southWestCorner,
                    Location northEastCorner)
                 {
                     FacilityLocationProblem problem;
                     problem.facilities = std::move(facilities);
                     problem.consumers = std::move(consumers);
                     problem.southWestCorner = southWestCorner;
                     problem.northEastCorner = northEastCorner;
                     return problem;
                 }),
             py::arg("facilities"),
             py::arg("consumers"),
             py::arg("south_west_corner") = Location{-90.0, -180.0},
             py::arg("north_east_corner") = Location{90.0, 180.0})
        // NOTE: element access returns copies; use set_assignment for bulk
        // facility writes.
        .def_readwrite("facilities", &FacilityLocationProblem::facilities)
        .def_readwrite("consumers", &FacilityLocationProblem::consumers)
        .def_readwrite("south_west_corner",
                       &FacilityLocationProblem::southWestCorner)
        .def_readwrite("north_east_corner",
                       &FacilityLocationProblem::northEastCorner)
        // Constraint weights (see fl/domain.hpp): setup_cost_weight mirrors
        // the provider's fixed ofSoft(2); distance_from_facility_weight
        // defaults to 6 = the quickstart solution's ConstraintWeightOverrides
        // value (the provider's un-overridden default is 5).
        .def_readwrite("setup_cost_weight",
                       &FacilityLocationProblem::setupCostWeight)
        .def_readwrite("distance_from_facility_weight",
                       &FacilityLocationProblem::distanceFromFacilityWeight)
        .def_readonly("score", &FacilityLocationProblem::score)
        .def("facility_count", &FacilityLocationProblem::facilityCount)
        .def("consumer_count", &FacilityLocationProblem::consumerCount)
        // Summary mirrors of the Java solution's getters.
        .def("total_cost", &FacilityLocationProblem::totalCost)
        .def("potential_cost", &FacilityLocationProblem::potentialCost)
        .def("total_distance_meters",
             &FacilityLocationProblem::totalDistanceMeters)
        .def("used_facility_count", &FacilityLocationProblem::usedFacilityCount)
        // Convenience: the current facility per consumer (the solution).
        .def("assignment",
             [](const FacilityLocationProblem &problem)
             {
                 std::vector<int> a;
                 a.reserve(problem.consumers.size());
                 for (const Consumer &consumer : problem.consumers)
                     a.push_back(consumer.facility);
                 return a;
             })
        // Bulk write of the planning variable (consumer access via
        // .consumers returns copies, so per-consumer writes would be lost).
        .def(
            "set_assignment",
            [](FacilityLocationProblem &problem, const std::vector<int> &a)
            {
                if (a.size() != problem.consumers.size())
                    throw py::value_error(
                        "assignment length must equal the consumer count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    problem.consumers[i].facility = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("problem"),
          "Full recompute of the HardSoftScore (also stores problem.score).");

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
        // 0 = auto: the solver scales the history with the instance size
        // (clamp(8 * consumers, 400, 3200)).
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
        .def_property(
            "ruin_recreate_rewards",
            [](const Solver::Config &c) { return c.ruinRecreateRewards; },
            [](Solver::Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error(
                        "ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruinRecreate.asMoveWeight). 0 disables it.
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Nearby selection over consumers (geo distance): swap-partner bias
        // and LNS related-ruin upgrade. probability 0 = off.
        .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
        .def_readwrite("nearby_size", &Solver::Config::nearbySize)
        // Guided change-move targets from each consumer's k nearest
        // facilities. probability 0 = off.
        .def_readwrite("value_recommender_probability",
                       &Solver::Config::valueRecommenderProbability)
        .def_readwrite("value_recommender_keep",
                       &Solver::Config::valueRecommenderKeep)
        // BASIC best-improvement descent strategy weight. 0 = off.
        .def_readwrite("descent_weight", &Solver::Config::descentWeight)
        // Gate on how often a step may draw CloseFacilityMove. 1 = engine
        // default (unrestricted).
        .def_readwrite("custom_move_probability",
                       &Solver::Config::customMoveProbability)
        // CloseFacilityMove: the domain macro move that takes one facility
        // out of service in a single step (see cpp/src/solver.cpp).
        .def_readwrite("close_facility_moves",
                       &Solver::Config::closeFacilityMoves)
        .def_readwrite("close_facility_keep",
                       &Solver::Config::closeFacilityKeep)
        .def_readwrite("close_facility_migrate_probability",
                       &Solver::Config::closeFacilityMigrateProbability)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -1e18)
        // stops at the first feasible solution.
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
        // Solves in place: sets consumer.facility and problem.score. The GIL
        // is released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<FacilityLocationProblem &>(&Solver::solve),
             py::arg("problem"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // facility_location.constraints). The picks replace the defaults;
        // None keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               FacilityLocationProblem &problem,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(problem);
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(problem), constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(problem, calculator);
            },
            py::arg("problem"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `problem` — the input
    // facility_location.constraints.score() turns into a score. Scoring once
    // is enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](FacilityLocationProblem &problem)
        {
            ScoreCalculator::prepare(problem);
            DecomposedCalculator calculator;
            defaultConstraints(problem).into(calculator);
            return tfpy::constraintCosts(calculator, problem);
        },
        py::arg("problem"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        []
        {
            FacilityLocationProblem empty;
            return tfpy::constraintNames(defaultConstraints(empty));
        },
        "Constraint names available to facility_location.constraints.");
}
