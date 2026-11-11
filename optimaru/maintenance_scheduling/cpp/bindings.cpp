// pybind11 bindings for the maintenance-scheduling C++ domain wrappers.
// The Python extension links the existing mnt::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
// is not modified here.
//
// Dates cross the boundary as epoch days (days since 1970-01-01);
// the pydantic mirrors in
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
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace mnt;
namespace opy = optimaru::python;

namespace {

std::optional<std::int64_t> dateToPy(std::int64_t date) {
    if (date == kNoDate) return std::nullopt;
    return date;
}

std::int64_t dateFromPy(const std::optional<std::int64_t>& date) {
    return date.has_value() ? *date : kNoDate;
}

// Binds one optional-date field (epoch day <-> None).
template <typename Class>
void defOptionalDate(py::class_<Class>& cls, const char* name, std::int64_t Class::* member,
                     const char* doc) {
    cls.def_property(
            name, [member](const Class& obj) { return dateToPy(obj.*member); },
            [member](Class& obj, const std::optional<std::int64_t>& value) {
                obj.*member = dateFromPy(value);
            },
            doc);
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Maintenance scheduling — optimaru engine-backed C++ wrapper "
            "exposed to Python";

    opy::bind_score<Score>(m);

    py::class_<Crew>(m, "Crew", py::module_local())
            .def(py::init([](std::string id, std::string name) {
                     Crew c;
                     c.id = std::move(id);
                     c.name = std::move(name);
                     return c;
                 }),
                 py::arg("id"), py::arg("name"))
            .def_readwrite("id", &Crew::id)
            .def_readwrite("name", &Crew::name);

    py::class_<Job> job(m, "Job", py::module_local());
    job.def(py::init([](std::string id, std::string name, int durationInDays,
                        const std::optional<std::int64_t>& minStartDate,
                        const std::optional<std::int64_t>& maxEndDate,
                        const std::optional<std::int64_t>& idealEndDate,
                        std::vector<std::string> tags) {
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
            py::arg("id"), py::arg("name"), py::arg("duration_in_days"),
            py::arg("min_start_date") = py::none(), py::arg("max_end_date") = py::none(),
            py::arg("ideal_end_date") = py::none(), py::arg("tags") = std::vector<std::string>{})
            .def_readwrite("id", &Job::id)
            .def_readwrite("name", &Job::name)
            .def_readwrite("duration_in_days", &Job::durationInDays)
            .def_readwrite("tags", &Job::tags)
            .def_readwrite("crew_date", &Job::crewDate)
            .def_readwrite("pinned", &Job::pinned);
    defOptionalDate(job, "min_start_date", &Job::minStartDate,
                    "Inclusive epoch day; None = no minimum.");
    defOptionalDate(job, "max_end_date", &Job::maxEndDate,
                    "Exclusive epoch day; None = no maximum.");
    defOptionalDate(job, "ideal_end_date", &Job::idealEndDate,
                    "Exclusive epoch day; None = no ideal end.");

    py::class_<MaintenanceSchedule>(m, "MaintenanceSchedule", py::module_local())
            .def(py::init([](std::int64_t fromDate, std::int64_t toDate, std::vector<Crew> crews,
                             std::vector<Job> jobs) {
                     MaintenanceSchedule s;
                     s.fromDate = fromDate;
                     s.toDate = toDate;
                     s.crews = std::move(crews);
                     s.jobs = std::move(jobs);
                     return s;
                 }),
                 py::arg("from_date"), py::arg("to_date"), py::arg("crews"), py::arg("jobs"))
            .def_readwrite("from_date", &MaintenanceSchedule::fromDate)
            .def_readwrite("to_date", &MaintenanceSchedule::toDate)
            .def_readwrite("crews", &MaintenanceSchedule::crews)
            .def_readwrite("jobs", &MaintenanceSchedule::jobs)
            .def_readonly("score", &MaintenanceSchedule::score)
            // composite crewDate encoding helpers (see mnt/domain.hpp)
            .def("crew_count", &MaintenanceSchedule::crewCount)
            .def(
                    "date_count",
                    [](MaintenanceSchedule& s) {
                        ScoreCalculator::prepare(s);
                        return s.dateCount();
                    },
                    "Number of start dates (business days of the calendar).")
            .def("crew_date_count",
                 [](MaintenanceSchedule& s) {
                     ScoreCalculator::prepare(s);
                     return s.crewDateCount();
                 })
            .def(
                    "crew_date_of",
                    [](MaintenanceSchedule& s, int dateIndex, int crewIndex) {
                        ScoreCalculator::prepare(s);
                        return s.crewDateOf(dateIndex, crewIndex);
                    },
                    py::arg("date_index"), py::arg("crew_index"))
            .def("date_index_of", &MaintenanceSchedule::dateIndexOf, py::arg("crew_date"))
            .def("crew_index_of", &MaintenanceSchedule::crewIndexOf, py::arg("crew_date"))
            .def(
                    "start_dates",
                    [](MaintenanceSchedule& s) {
                        ScoreCalculator::prepare(s);
                        return s.startDates;
                    },
                    "The start-date value range: every non-weekend epoch day in "
                    "[from_date, to_date).")
            .def(
                    "job_start_date",
                    [](MaintenanceSchedule& s, int jobIndex) -> std::optional<std::int64_t> {
                        ScoreCalculator::prepare(s);
                        const Job& job = s.jobs.at(jobIndex);
                        if (job.crewDate < 0) return std::nullopt;
                        return s.startDates[s.dateIndexOf(job.crewDate)];
                    },
                    py::arg("job_index"), "The assigned start epoch day, or None if unassigned.")
            .def(
                    "job_end_date",
                    [](MaintenanceSchedule& s, int jobIndex) -> std::optional<std::int64_t> {
                        ScoreCalculator::prepare(s);
                        const Job& job = s.jobs.at(jobIndex);
                        if (job.crewDate < 0) return std::nullopt;
                        return s.endDateFor(job.durationInDays, s.dateIndexOf(job.crewDate));
                    },
                    py::arg("job_index"),
                    "The derived end date (exclusive epoch day; weekends skipped), or "
                    "None if unassigned.")
            // Convenience: the current crewDate per job (the solution).
            .def("assignment",
                 [](const MaintenanceSchedule& s) {
                     std::vector<int> a;
                     a.reserve(s.jobs.size());
                     for (const Job& job : s.jobs) a.push_back(job.crewDate);
                     return a;
                 })
            // Bulk write of the decision variable (job access via .jobs returns
            // copies, so per-job writes would be lost).
            .def(
                    "set_assignment",
                    [](MaintenanceSchedule& s, const std::vector<int>& a) {
                        if (a.size() != s.jobs.size())
                            throw py::value_error("assignment length must equal the job count");
                        for (std::size_t i = 0; i < a.size(); ++i) s.jobs[i].crewDate = a[i];
                    },
                    py::arg("assignment"));

    m.def("calculate_score", &ScoreCalculator::score, py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");

    m.def("plus_business_days", &plusBusinessDays, py::arg("start_date"), py::arg("business_days"),
          "The exclusive end epoch day after "
          "walking `business_days` non-weekend days from `start_date`.");

    m.def(
            "generate_demo_data",
            [](const std::string& size, std::optional<std::int64_t> fromDate) {
                DemoData demoData;
                if (size == "SMALL")
                    demoData = DemoData::SMALL;
                else if (size == "LARGE")
                    demoData = DemoData::LARGE;
                else
                    throw py::value_error("size must be 'SMALL' or 'LARGE'");
                const std::int64_t from =
                        fromDate.has_value() ? *fromDate : nextMonday(todayEpochDay());
                return generateDemoData(demoData, from);
            },
            py::arg("size") = "SMALL", py::arg("from_date") = py::none(),
            "A demo instance (deterministic, seed 17). from_date defaults to "
            "the next Monday; pass a fixed Monday epoch day for reproducibility.");

    py::class_<Solver::Config> config(m, "SolverConfig", py::module_local());

    optimaru::python::bind_solver_config(config);
    config.def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Guided selection (see cpp/src/solver.cpp): change moves draw the
            // start date from the job's own [minStart, idealEnd] window, seed on
            // jobs sitting in a tagConflictCost, and prefer a crew that is free
            // for the whole span.
            .def_readwrite("window_value_probability", &Solver::Config::window_value_probability)
            .def_readwrite("hot_job_probability", &Solver::Config::hot_job_probability)
            .def_readwrite("free_crew_candidates", &Solver::Config::free_crew_candidates);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets job.crew_date and schedule.score. The GIL is
            // released so parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<MaintenanceSchedule&>(&Solver::solve),
                 py::arg("schedule"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.maintenance_scheduling.constraints). The picks replace the defaults;
            // None keeps them. A name may be picked several times: every pick is
            // its own instance (a fresh copy of the default component) with its
            // own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, MaintenanceSchedule& schedule,
                       const py::object& constraints) {
                        ScoreCalculator::prepare(schedule);
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(schedule, calculator);
                    },
                    py::arg("schedule"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly.
    opy::bind_constraints<MaintenanceSchedule>(
            m, "schedule", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
