// pybind11 bindings for the maintenance-scheduling C++ domain wrappers.
// The Python extension links the existing mnt::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here.
//
// Dates cross the boundary as epoch days (days since 1970-01-01, Java's
// LocalDate.toEpochDay); the pydantic mirrors in
// maintenance_scheduling/models.py convert to/from datetime.date. The
// optional min/max/ideal dates are std::optional here (None <-> mnt::kNoDate).
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional conversions

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mnt/constraints.hpp"
#include "mnt/demo_data.hpp"
#include "mnt/domain.hpp"
#include "mnt/score_calculator.hpp"
#include "mnt/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace mnt;
namespace tfpy = optimaru::python;

namespace
{

std::optional<std::int64_t> dateToPy(std::int64_t date)
{
    if (date == kNoDate)
        return std::nullopt;
    return date;
}

std::int64_t dateFromPy(const std::optional<std::int64_t> &date)
{
    return date.has_value() ? *date : kNoDate;
}

// Binds one optional-date field (epoch day <-> None).
template <typename Class>
void defOptionalDate(py::class_<Class> &cls,
                     const char *name,
                     std::int64_t Class::*member,
                     const char *doc)
{
    cls.def_property(
        name,
        [member](const Class &obj) { return dateToPy(obj.*member); },
        [member](Class &obj, const std::optional<std::int64_t> &value)
        { obj.*member = dateFromPy(value); },
        doc);
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Maintenance scheduling — Timefold engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Crew>(m, "Crew", py::module_local())
        .def(py::init(
                 [](std::string id, std::string name)
                 {
                     Crew c;
                     c.id = std::move(id);
                     c.name = std::move(name);
                     return c;
                 }),
             py::arg("id"),
             py::arg("name"))
        .def_readwrite("id", &Crew::id)
        .def_readwrite("name", &Crew::name);

    py::class_<Job> job(m, "Job", py::module_local());
    job.def(py::init(
                [](std::string id,
                   std::string name,
                   int durationInDays,
                   const std::optional<std::int64_t> &minStartDate,
                   const std::optional<std::int64_t> &maxEndDate,
                   const std::optional<std::int64_t> &idealEndDate,
                   std::vector<std::string> tags)
                {
                    Job j;
                    j.id = std::move(id);
                    j.name = std::move(name);
                    j.durationInDays = durationInDays;
                    j.minStartDate = dateFromPy(minStartDate);
                    j.maxEndDate = dateFromPy(maxEndDate);
                    j.idealEndDate = dateFromPy(idealEndDate);
                    j.tags = std::move(tags);
                    return j;
                }),
            py::arg("id"),
            py::arg("name"),
            py::arg("duration_in_days"),
            py::arg("min_start_date") = py::none(),
            py::arg("max_end_date") = py::none(),
            py::arg("ideal_end_date") = py::none(),
            py::arg("tags") = std::vector<std::string>{})
        .def_readwrite("id", &Job::id)
        .def_readwrite("name", &Job::name)
        .def_readwrite("duration_in_days", &Job::durationInDays)
        .def_readwrite("tags", &Job::tags)
        .def_readwrite("crew_date", &Job::crewDate)
        .def_readwrite("pinned", &Job::pinned);
    defOptionalDate(job,
                    "min_start_date",
                    &Job::minStartDate,
                    "Inclusive epoch day; None = no minimum.");
    defOptionalDate(job,
                    "max_end_date",
                    &Job::maxEndDate,
                    "Exclusive epoch day; None = no maximum.");
    defOptionalDate(job,
                    "ideal_end_date",
                    &Job::idealEndDate,
                    "Exclusive epoch day; None = no ideal end.");

    py::class_<MaintenanceSchedule>(m, "MaintenanceSchedule", py::module_local())
        .def(py::init(
                 [](std::int64_t fromDate,
                    std::int64_t toDate,
                    std::vector<Crew> crews,
                    std::vector<Job> jobs)
                 {
                     MaintenanceSchedule s;
                     s.fromDate = fromDate;
                     s.toDate = toDate;
                     s.crews = std::move(crews);
                     s.jobs = std::move(jobs);
                     return s;
                 }),
             py::arg("from_date"),
             py::arg("to_date"),
             py::arg("crews"),
             py::arg("jobs"))
        .def_readwrite("from_date", &MaintenanceSchedule::fromDate)
        .def_readwrite("to_date", &MaintenanceSchedule::toDate)
        .def_readwrite("crews", &MaintenanceSchedule::crews)
        .def_readwrite("jobs", &MaintenanceSchedule::jobs)
        .def_readonly("score", &MaintenanceSchedule::score)
        // composite crewDate encoding helpers (see mnt/domain.hpp)
        .def("crew_count", &MaintenanceSchedule::crewCount)
        .def(
            "date_count",
            [](MaintenanceSchedule &s)
            {
                ScoreCalculator::prepare(s);
                return s.dateCount();
            },
            "Number of start dates (business days of the calendar).")
        .def("crew_date_count",
             [](MaintenanceSchedule &s)
             {
                 ScoreCalculator::prepare(s);
                 return s.crewDateCount();
             })
        .def(
            "crew_date_of",
            [](MaintenanceSchedule &s, int dateIndex, int crewIndex)
            {
                ScoreCalculator::prepare(s);
                return s.crewDateOf(dateIndex, crewIndex);
            },
            py::arg("date_index"),
            py::arg("crew_index"))
        .def("date_index_of",
             &MaintenanceSchedule::dateIndexOf,
             py::arg("crew_date"))
        .def("crew_index_of",
             &MaintenanceSchedule::crewIndexOf,
             py::arg("crew_date"))
        .def(
            "start_dates",
            [](MaintenanceSchedule &s)
            {
                ScoreCalculator::prepare(s);
                return s.startDates;
            },
            "The start-date value range: every non-weekend epoch day in "
            "[from_date, to_date) — Java's createStartDateList().")
        .def(
            "job_start_date",
            [](MaintenanceSchedule &s,
               int jobIndex) -> std::optional<std::int64_t>
            {
                ScoreCalculator::prepare(s);
                const Job &job = s.jobs.at(jobIndex);
                if (job.crewDate < 0)
                    return std::nullopt;
                return s.startDates[s.dateIndexOf(job.crewDate)];
            },
            py::arg("job_index"),
            "The assigned start epoch day, or None if unassigned.")
        .def(
            "job_end_date",
            [](MaintenanceSchedule &s,
               int jobIndex) -> std::optional<std::int64_t>
            {
                ScoreCalculator::prepare(s);
                const Job &job = s.jobs.at(jobIndex);
                if (job.crewDate < 0)
                    return std::nullopt;
                return s.endDateFor(job.durationInDays,
                                    s.dateIndexOf(job.crewDate));
            },
            py::arg("job_index"),
            "The shadow endDate (exclusive epoch day; weekends skipped), or "
            "None if unassigned.")
        // Convenience: the current crewDate per job (the solution).
        .def("assignment",
             [](const MaintenanceSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.jobs.size());
                 for (const Job &job : s.jobs)
                     a.push_back(job.crewDate);
                 return a;
             })
        // Bulk write of the planning variable (job access via .jobs returns
        // copies, so per-job writes would be lost).
        .def(
            "set_assignment",
            [](MaintenanceSchedule &s, const std::vector<int> &a)
            {
                if (a.size() != s.jobs.size())
                    throw py::value_error(
                        "assignment length must equal the job count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    s.jobs[i].crewDate = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");

    m.def("plus_business_days",
          &plusBusinessDays,
          py::arg("start_date"),
          py::arg("business_days"),
          "Java Job.calculateEndDate: the exclusive end epoch day after "
          "walking `business_days` non-weekend days from `start_date`.");

    m.def(
        "generate_demo_data",
        [](const std::string &size, std::optional<std::int64_t> fromDate)
        {
            DemoData demoData;
            if (size == "SMALL")
                demoData = DemoData::SMALL;
            else if (size == "LARGE")
                demoData = DemoData::LARGE;
            else
                throw py::value_error("size must be 'SMALL' or 'LARGE'");
            const std::int64_t from = fromDate.has_value()
                                          ? *fromDate
                                          : nextMonday(todayEpochDay());
            return generateDemoData(demoData, from);
        },
        py::arg("size") = "SMALL",
        py::arg("from_date") = py::none(),
        "The Java DemoDataGenerator instance (bit-exact java.util.Random "
        "replica). from_date defaults to the next Monday, like the Java "
        "demo; pass a fixed Monday epoch day for reproducibility.");

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
        // 0 = auto: clamp(8 * jobs, 400, 3200).
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
        // Guided selection (see cpp/src/solver.cpp): change moves draw the
        // start date from the job's own [minStart, idealEnd] window, seed on
        // jobs sitting in a tagConflict, and prefer a crew that is free for
        // the whole span.
        .def_readwrite("window_value_probability",
                       &Solver::Config::windowValueProbability)
        .def_readwrite("hot_job_probability",
                       &Solver::Config::hotJobProbability)
        .def_readwrite("free_crew_candidates",
                       &Solver::Config::freeCrewCandidates)
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
        // Callback fired on every new best solution: on_new_best(step,
        // score). The pybind11 wrapper re-acquires the GIL, so it is safe
        // from the engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets job.crew_date and schedule.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<MaintenanceSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // maintenance_scheduling.constraints). The picks replace the defaults;
        // None keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               MaintenanceSchedule &schedule,
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
    // maintenance_scheduling.constraints.score() turns into a score. Scoring
    // once is enough: re-pricing any assembly is pure Python arithmetic on
    // these.
    m.def(
        "constraint_costs",
        [](MaintenanceSchedule &schedule)
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
        "Constraint names available to maintenance_scheduling.constraints.");
}
