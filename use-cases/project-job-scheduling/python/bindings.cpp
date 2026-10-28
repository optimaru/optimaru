// pybind11 bindings for the project-job-scheduling C++ domain wrappers.
// The Python extension links the existing pjs::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here. One domain is exposed: the quickstart port
// (pjs::ProjectJobSchedule, BASIC composite (executionMode, delay) variable),
// as the module project_job._core.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string

#include <chrono>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "pjs/constraints.hpp"
#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "pjs/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace pjs;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Project job scheduling (multi-project RCPSP with execution "
              "modes) — Timefold engine-backed C++ wrapper exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("medium", &Score::medium)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::enum_<JobType>(m, "JobType", py::module_local())
        .value("SOURCE", JobType::SOURCE)
        .value("STANDARD", JobType::STANDARD)
        .value("SINK", JobType::SINK);

    py::class_<Project>(m, "Project", py::module_local())
        .def(py::init(
                 [](std::string id, int releaseDate, int criticalPathDuration)
                 {
                     Project p;
                     p.id = std::move(id);
                     p.releaseDate = releaseDate;
                     p.criticalPathDuration = criticalPathDuration;
                     return p;
                 }),
             py::arg("id"),
             py::arg("release_date") = 0,
             py::arg("critical_path_duration") = 0)
        .def_readwrite("id", &Project::id)
        .def_readwrite("release_date", &Project::releaseDate)
        .def_readwrite("critical_path_duration", &Project::criticalPathDuration)
        .def("critical_path_end_date", &Project::criticalPathEndDate);

    py::class_<Resource>(m, "Resource", py::module_local())
        .def(py::init(
                 [](std::string id, int capacity, bool renewable, int project)
                 {
                     Resource r;
                     r.id = std::move(id);
                     r.capacity = capacity;
                     r.renewable = renewable;
                     r.project = project;
                     return r;
                 }),
             py::arg("id"),
             py::arg("capacity"),
             py::arg("renewable") = true,
             py::arg("project") = -1)
        .def_readwrite("id", &Resource::id)
        .def_readwrite("capacity", &Resource::capacity)
        // renewable: capacity per day; non-renewable: over the whole horizon.
        .def_readwrite("renewable", &Resource::renewable)
        // Index into ProjectJobSchedule.projects, -1 = global (informational).
        .def_readwrite("project", &Resource::project);

    py::class_<ResourceRequirement>(m, "ResourceRequirement", py::module_local())
        .def(py::init(
                 [](int resource, int requirement)
                 {
                     ResourceRequirement r;
                     r.resource = resource;
                     r.requirement = requirement;
                     return r;
                 }),
             py::arg("resource"),
             py::arg("requirement"))
        // Index into ProjectJobSchedule.resources.
        .def_readwrite("resource", &ResourceRequirement::resource)
        .def_readwrite("requirement", &ResourceRequirement::requirement);

    py::class_<ExecutionMode>(m, "ExecutionMode", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int duration,
                    std::vector<ResourceRequirement> resourceRequirements)
                 {
                     ExecutionMode e;
                     e.id = std::move(id);
                     e.duration = duration;
                     e.resourceRequirements = std::move(resourceRequirements);
                     return e;
                 }),
             py::arg("id"),
             py::arg("duration") = 0,
             py::arg("resource_requirements")
             = std::vector<ResourceRequirement>{})
        .def_readwrite("id", &ExecutionMode::id)
        .def_readwrite("duration", &ExecutionMode::duration)
        .def_readwrite("resource_requirements",
                       &ExecutionMode::resourceRequirements);

    py::class_<Job>(m, "Job", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int project,
                    JobType jobType,
                    std::vector<ExecutionMode> executionModes)
                 {
                     Job j;
                     j.id = std::move(id);
                     j.project = project;
                     j.jobType = jobType;
                     j.executionModes = std::move(executionModes);
                     return j;
                 }),
             py::arg("id"),
             py::arg("project"),
             py::arg("job_type") = JobType::STANDARD,
             py::arg("execution_modes") = std::vector<ExecutionMode>{})
        .def_readwrite("id", &Job::id)
        // Index into ProjectJobSchedule.projects.
        .def_readwrite("project", &Job::project)
        .def_readwrite("job_type", &Job::jobType)
        .def_readwrite("execution_modes", &Job::executionModes);

    py::class_<Allocation>(m, "Allocation", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int job,
                    std::vector<int> predecessorAllocations,
                    int modeDelay,
                    bool pinned)
                 {
                     Allocation a;
                     a.id = std::move(id);
                     a.job = job;
                     a.predecessorAllocations
                         = std::move(predecessorAllocations);
                     a.modeDelay = modeDelay;
                     a.pinned = pinned;
                     return a;
                 }),
             py::arg("id"),
             py::arg("job"),
             py::arg("predecessor_allocations") = std::vector<int>{},
             py::arg("mode_delay") = -1,
             py::arg("pinned") = false)
        .def_readwrite("id", &Allocation::id)
        // Index into ProjectJobSchedule.jobs (1 allocation per job).
        .def_readwrite("job", &Allocation::job)
        .def_readwrite("predecessor_allocations",
                       &Allocation::predecessorAllocations)
        // The composite planning variable: -1 unassigned, otherwise
        // delay * max_mode_count() + mode_slot (see the module docstring).
        .def_readwrite("mode_delay", &Allocation::modeDelay)
        // SOURCE/SINK allocations are always pinned; this flag additionally
        // pins a STANDARD allocation.
        .def_readwrite("pinned", &Allocation::pinned)
        // Derived dates (the Java shadow variables), refreshed by
        // calculate_score / Solver.solve.
        .def_readonly("predecessors_done_date",
                      &Allocation::predecessorsDoneDate)
        .def_readonly("start_date", &Allocation::startDate)
        .def_readonly("end_date", &Allocation::endDate);

    py::class_<ProjectJobSchedule>(m, "ProjectJobSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Project> projects,
                    std::vector<Resource> resources,
                    std::vector<Job> jobs,
                    std::vector<Allocation> allocations,
                    int delayRangeSize)
                 {
                     ProjectJobSchedule schedule;
                     schedule.projects = std::move(projects);
                     schedule.resources = std::move(resources);
                     schedule.jobs = std::move(jobs);
                     schedule.allocations = std::move(allocations);
                     schedule.delayRangeSize = delayRangeSize;
                     // Validates the instance (throws ValueError on bad
                     // indices, modeless jobs or precedence cycles) and
                     // computes max_mode_count / the topological order.
                     ScoreCalculator::prepare(schedule);
                     return schedule;
                 }),
             py::arg("projects"),
             py::arg("resources"),
             py::arg("jobs"),
             py::arg("allocations"),
             py::arg("delay_range_size") = 500)
        .def_readonly("projects", &ProjectJobSchedule::projects)
        .def_readonly("resources", &ProjectJobSchedule::resources)
        .def_readonly("jobs", &ProjectJobSchedule::jobs)
        // NOTE: element access returns copies; use set_assignment for bulk
        // mode_delay writes.
        .def_readonly("allocations", &ProjectJobSchedule::allocations)
        .def_readonly("delay_range_size", &ProjectJobSchedule::delayRangeSize)
        .def_readonly("score", &ProjectJobSchedule::score)
        .def("allocation_count", &ProjectJobSchedule::allocationCount)
        .def_readonly("max_mode_count", &ProjectJobSchedule::maxModeCount)
        .def("value_count", &ProjectJobSchedule::valueCount)
        // --- composite mode_delay encoding helpers ---
        .def(
            "encode_value",
            [](const ProjectJobSchedule &s, int delay, int modeSlot)
            {
                if (delay < 0 || delay >= s.delayRangeSize)
                    throw py::value_error("delay out of range");
                if (modeSlot < 0 || modeSlot >= s.maxModeCount)
                    throw py::value_error("mode_slot out of range");
                return s.valueOf(delay, modeSlot);
            },
            py::arg("delay"),
            py::arg("mode_slot"),
            "delay * max_mode_count + mode_slot; a mode INDEX of the job is "
            "always a valid slot.")
        .def("delay_of_value",
             &ProjectJobSchedule::delayOfValue,
             py::arg("value"))
        .def("mode_slot_of_value",
             &ProjectJobSchedule::modeSlotOfValue,
             py::arg("value"))
        .def("execution_mode_index_of_value",
             &ProjectJobSchedule::executionModeIndexOfValue,
             py::arg("allocation"),
             py::arg("value"),
             "The decoded execution mode index (slot modulo the job's own "
             "mode count).")
        // Convenience: the current mode_delay value per allocation.
        .def("assignment",
             [](const ProjectJobSchedule &schedule)
             {
                 std::vector<int> assignment;
                 assignment.reserve(schedule.allocations.size());
                 for (const Allocation &allocation : schedule.allocations)
                     assignment.push_back(allocation.modeDelay);
                 return assignment;
             })
        // Bulk write of the planning variable (allocation access via
        // .allocations returns copies, so per-allocation writes would be
        // lost).
        .def(
            "set_assignment",
            [](ProjectJobSchedule &schedule, const std::vector<int> &assignment)
            {
                if (assignment.size() != schedule.allocations.size())
                    throw py::value_error(
                        "assignment length must equal the allocation count");
                for (std::size_t i = 0; i < assignment.size(); ++i)
                    schedule.allocations[i].modeDelay = assignment[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardMediumSoft score; also refreshes every "
          "allocation's derived dates and stores schedule.score.");

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
        // 0 = auto: clamp(8 * allocation count, 400, 3200).
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
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Early termination once bestScore >= (hard, medium, soft), e.g.
        // (0, -2**62, -2**62) stops at the first feasible solution.
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
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets allocation.mode_delay, the derived dates and
        // schedule.score. The GIL is released so parallel multi-start
        // workers can fire on_new_best.
        .def("solve",
             py::overload_cast<ProjectJobSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // project_job.constraints). The picks replace the defaults; None
        // keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               ProjectJobSchedule &schedule,
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
    // project_job.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](ProjectJobSchedule &schedule)
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
        "Constraint names available to project_job.constraints.");
}
