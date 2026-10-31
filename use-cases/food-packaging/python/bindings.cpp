// pybind11 bindings for the food-packaging C++ domain wrappers. The Python
// extension links the existing fp::* implementations and imports the generic
// optimaru engine from ../../../include; the engine itself is not
// modified here.
//
// Times cross the boundary as int64 MINUTES from a common reference (the
// demo instance uses its Monday 00:00 as minute 0; the pydantic mirrors in
// food_packaging/models.py use epoch minutes and convert to/from
// datetime.datetime). The shadow times are exposed as Optional[int]
// (None <-> fp::kNoTime, Java's null LocalDateTime).
#include <pybind11/functional.h>  // std::function <-> Python callable
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fp/constraints.hpp"
#include "fp/demo_data.hpp"
#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "fp/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace fp;
namespace tfpy = optimaru::python;

namespace
{

std::optional<std::int64_t> timeToPy(Minutes minute)
{
    if (minute == kNoTime)
        return std::nullopt;
    return minute;
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Food packaging — optimaru engine-backed C++ wrapper exposed "
              "to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def(py::init(
                 [](std::int64_t hard, std::int64_t medium, std::int64_t soft)
                 { return Score{hard, medium, soft}; }),
             py::arg("hard") = 0,
             py::arg("medium") = 0,
             py::arg("soft") = 0)
        .def_readonly("hard", &Score::hard)
        .def_readonly("medium", &Score::medium)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__eq__", [](const Score &a, const Score &b) { return a == b; })
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<ConstraintBreakdown>(m, "ConstraintBreakdown", py::module_local())
        .def_readonly("max_end_date_time", &ConstraintBreakdown::maxEndDateTime)
        .def_readonly("operator_cleaning_conflict",
                      &ConstraintBreakdown::operatorCleaningConflict)
        .def_readonly("ideal_end_date_time",
                      &ConstraintBreakdown::idealEndDateTime)
        .def_readonly("maximize_jobs_assigned",
                      &ConstraintBreakdown::maximizeJobsAssigned)
        .def_readonly("minimize_makespan",
                      &ConstraintBreakdown::minimizeMakespan)
        .def("to_score", &ConstraintBreakdown::toScore);

    py::class_<Product>(m, "Product", py::module_local())
        .def(py::init(
                 [](std::string id, std::string name)
                 {
                     Product p;
                     p.id = std::move(id);
                     p.name = std::move(name);
                     return p;
                 }),
             py::arg("id"),
             py::arg("name"))
        .def_readwrite("id", &Product::id)
        .def_readwrite("name", &Product::name);

    py::class_<Operator>(m, "Operator", py::module_local())
        .def(py::init([](std::string id) { return Operator{std::move(id)}; }),
             py::arg("id"))
        .def_readwrite("id", &Operator::id);

    py::class_<Job> job(m, "Job", py::module_local());
    job.def(py::init(
                [](std::string id,
                   std::string name,
                   int productIndex,
                   std::int64_t durationMinutes,
                   std::int64_t minStartMinute,
                   std::int64_t idealEndMinute,
                   std::int64_t maxEndMinute,
                   int priority,
                   bool pinned)
                {
                    Job j;
                    j.id = std::move(id);
                    j.name = std::move(name);
                    j.productIndex = productIndex;
                    j.durationMinutes = durationMinutes;
                    j.minStartTime = minStartMinute;
                    j.idealEndTime = idealEndMinute;
                    j.maxEndTime = maxEndMinute;
                    j.priority = priority;
                    j.pinned = pinned;
                    return j;
                }),
            py::arg("id"),
            py::arg("name"),
            py::arg("product_index"),
            py::arg("duration_minutes"),
            py::arg("min_start_minute"),
            py::arg("ideal_end_minute"),
            py::arg("max_end_minute"),
            py::arg("priority") = 1,
            py::arg("pinned") = false)
        .def_readwrite("id", &Job::id)
        .def_readwrite("name", &Job::name)
        .def_readwrite("product_index", &Job::productIndex)
        .def_readwrite("duration_minutes", &Job::durationMinutes)
        // Data only, like Java: no constraint reads min_start_minute or
        // priority.
        .def_readwrite("min_start_minute", &Job::minStartTime)
        .def_readwrite("ideal_end_minute", &Job::idealEndTime)
        .def_readwrite("max_end_minute", &Job::maxEndTime)
        .def_readwrite("priority", &Job::priority)
        .def_readwrite("pinned", &Job::pinned)
        // Shadow variables (results; refreshed by update_shadow_variables /
        // calculate_score / Solver.solve).
        .def_readonly("line_index", &Job::lineIndex)
        .def_property_readonly("start_cleaning_minute",
                               [](const Job &j)
                               { return timeToPy(j.startCleaningDateTime); })
        .def_property_readonly("start_production_minute",
                               [](const Job &j)
                               { return timeToPy(j.startProductionDateTime); })
        .def_property_readonly(
            "end_minute", [](const Job &j) { return timeToPy(j.endDateTime); });

    py::class_<Line>(m, "Line", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    std::int64_t startMinute,
                    int operatorIndex,
                    std::vector<int> jobs)
                 {
                     Line l;
                     l.id = std::move(id);
                     l.name = std::move(name);
                     l.startDateTime = startMinute;
                     l.operatorIndex = operatorIndex;
                     l.jobs = std::move(jobs);
                     return l;
                 }),
             py::arg("id"),
             py::arg("name"),
             py::arg("start_minute"),
             py::arg("operator_index") = -1,
             py::arg("jobs") = std::vector<int>{})
        .def_readwrite("id", &Line::id)
        .def_readwrite("name", &Line::name)
        .def_readwrite("start_minute", &Line::startDateTime)
        .def_readwrite("operator_index", &Line::operatorIndex)
        // The @PlanningListVariable: ordered job indices.
        .def_readwrite("jobs", &Line::jobs);

    py::class_<PackagingSchedule>(m, "PackagingSchedule", py::module_local())
        .def(py::init(
                 [](std::int64_t fromMinute,
                    std::int64_t toMinute,
                    std::vector<Product> products,
                    std::vector<Minutes> cleanupMinutes,
                    std::vector<Operator> operators,
                    std::vector<Line> lines,
                    std::vector<Job> jobs,
                    std::vector<int> unassignedJobs)
                 {
                     PackagingSchedule s;
                     s.fromMinute = fromMinute;
                     s.toMinute = toMinute;
                     s.products = std::move(products);
                     s.cleanupMinutes = std::move(cleanupMinutes);
                     s.operators = std::move(operators);
                     s.lines = std::move(lines);
                     s.jobs = std::move(jobs);
                     s.unassignedJobs = std::move(unassignedJobs);
                     return s;
                 }),
             py::arg("from_minute"),
             py::arg("to_minute"),
             py::arg("products"),
             py::arg("cleanup_minutes"),
             py::arg("operators"),
             py::arg("lines"),
             py::arg("jobs"),
             py::arg("unassigned_jobs") = std::vector<int>{})
        .def_readwrite("from_minute", &PackagingSchedule::fromMinute)
        .def_readwrite("to_minute", &PackagingSchedule::toMinute)
        .def_readwrite("products", &PackagingSchedule::products)
        // Flat products x products matrix indexed [previous * count +
        // current]; -1 == missing entry (throws on first use, like Java).
        .def_readwrite("cleanup_minutes", &PackagingSchedule::cleanupMinutes)
        .def_readwrite("operators", &PackagingSchedule::operators)
        .def_readwrite("lines", &PackagingSchedule::lines)
        .def_readwrite("jobs", &PackagingSchedule::jobs)
        // The unassigned pool (the allowsUnassignedValues stand-in). Jobs in
        // no list at all are placed by the construction heuristic.
        .def_readwrite("unassigned_jobs", &PackagingSchedule::unassignedJobs)
        .def_readonly("score", &PackagingSchedule::score)
        .def("line_count", &PackagingSchedule::lineCount)
        .def(
            "cleanup_of",
            [](const PackagingSchedule &s, int previousProduct, int product)
            { return s.cleanup(previousProduct, product); },
            py::arg("previous_product"),
            py::arg("product"),
            "Cleanup minutes when `product` follows `previous_product`; -1 "
            "== missing entry.")
        .def("update_shadow_variables",
             &ScoreCalculator::updateShadowVariables,
             "Recompute every job's shadow variables (line_index, "
             "start_cleaning/start_production/end) from the current lists.");

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardMediumSoft score (also refreshes the "
          "shadow variables and stores schedule.score).");

    m.def("analyze",
          &ScoreCalculator::analyze,
          py::arg("schedule"),
          "Per-constraint penalty magnitudes of the current solution.");

    m.def("assign_missing_operators",
          &assignMissingOperators,
          py::arg("schedule"),
          "Give every operator-less line the least-used operator (the "
          "construction stand-in for the Java Line.operator variable).");

    m.def("improve_operator_assignment",
          &improveOperatorAssignment,
          py::arg("schedule"),
          "Hill-climb the line->operator assignment with the job lists "
          "fixed; returns whether anything changed.");

    m.def("generate_demo_data",
          &generateDemoData,
          py::arg("line_count") = 5,
          py::arg("job_count") = 100,
          "The Java DemoDataGenerator instance (bit-exact java.util.Random "
          "replica); minute 0 is the demo's Monday 00:00.");

    py::class_<Solver::Config> config(m, "SolverConfig", py::module_local());
    config.def(py::init<>())
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
                        "unimproved_spent_limit_seconds must be "
                        "non-negative");
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
        // 0 = auto: clamp(8 * unpinned jobs, 400, 3200).
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
        // 0 = auto: 1 without operator sharing, else 5 alternating rounds.
        .def_readwrite("operator_rounds", &Solver::Config::operatorRounds)
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
                        "ruin_recreate_rewards must contain exactly 4 "
                        "values");
                c.ruinRecreateRewards = std::move(rewards);
            })
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
        // Nearby selection over the cleanup matrix (cheap-changeover
        // neighborhoods); probability 0 disables.
        .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
        .def_readwrite("nearby_size", &Solver::Config::nearbySize)
        // Early termination once bestScore >= (hard, medium*1e10 + soft) as
        // the ENGINE score; (0, -1e18) stops at the first hard-feasible
        // solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            {
                c.bestScoreLimit = optimaru::HardSoftScore{hs.first, hs.second};
            })
        // Callback fired on every new best solution: on_new_best(step,
        // score) with the decoded HardMediumSoft score. The pybind11
        // wrapper re-acquires the GIL, so it is safe from the engine's
        // multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: mutates lines[*].jobs / unassigned_jobs (and
        // lines[*].operator_index when sharing forces alternation), then
        // refreshes the shadows and schedule.score. The GIL is released so
        // parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<PackagingSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // food_packaging.constraints). The picks replace the defaults; None
        // keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               PackagingSchedule &schedule,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `schedule` — the input
    // food_packaging.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](PackagingSchedule &schedule)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            return tfpy::constraintCosts(calculator, schedule);
        },
        py::arg("schedule"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraintNames(defaultConstraints()); },
        "Constraint names available to food_packaging.constraints.");
}
