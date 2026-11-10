// pybind11 bindings for the food-packaging C++ domain wrappers. The Python
// extension links the existing fp::* implementations and imports the generic
// optimaru engine from optimaru/cpp/optimaru; the engine itself is not
// modified here.
//
// Times cross the boundary as int64 MINUTES from a common reference (the
// demo instance uses its Monday 00:00 as minute 0; the pydantic mirrors in
// food_packaging/models.py use epoch minutes and convert to/from
// datetime.datetime). The derived times are exposed as Optional[int]
// (None <-> fp::kNoTime, "no time").
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
namespace opy = optimaru::python;

namespace {

std::optional<std::int64_t> timeToPy(Minutes minute) {
    if (minute == kNoTime) return std::nullopt;
    return minute;
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Food packaging — optimaru engine-backed C++ wrapper exposed "
            "to Python";

    py::class_<Score>(m, "Score", py::module_local())
            .def(py::init([](std::int64_t hard, std::int64_t medium, std::int64_t soft) {
                     return Score{hard, medium, soft};
                 }),
                 py::arg("hard") = 0, py::arg("medium") = 0, py::arg("soft") = 0)
            .def_readonly("hard", &Score::hard)
            .def_readonly("medium", &Score::medium)
            .def_readonly("soft", &Score::soft)
            .def("is_feasible", &Score::feasible)
            .def("__eq__", [](const Score& a, const Score& b) { return a == b; })
            .def("__str__", &Score::str)
            .def("__repr__", [](const Score& s) { return "<Score " + s.str() + ">"; });

    py::class_<Product>(m, "Product", py::module_local())
            .def(py::init([](std::string id, std::string name) {
                     Product p;
                     p.id = std::move(id);
                     p.name = std::move(name);
                     return p;
                 }),
                 py::arg("id"), py::arg("name"))
            .def_readwrite("id", &Product::id)
            .def_readwrite("name", &Product::name);

    py::class_<Operator>(m, "Operator", py::module_local())
            .def(py::init([](std::string id) { return Operator{std::move(id)}; }), py::arg("id"))
            .def_readwrite("id", &Operator::id);

    py::class_<Job> job(m, "Job", py::module_local());
    job.def(py::init([](std::string id, std::string name, int productIndex,
                        std::int64_t durationMinutes, std::int64_t minStartMinute,
                        std::int64_t idealEndMinute, std::int64_t maxEndMinute, int priority,
                        bool pinned) {
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
            py::arg("id"), py::arg("name"), py::arg("product_index"), py::arg("duration_minutes"),
            py::arg("min_start_minute"), py::arg("ideal_end_minute"), py::arg("max_end_minute"),
            py::arg("priority") = 1, py::arg("pinned") = false)
            .def_readwrite("id", &Job::id)
            .def_readwrite("name", &Job::name)
            .def_readwrite("product_index", &Job::productIndex)
            .def_readwrite("duration_minutes", &Job::durationMinutes)
            // Data only: no constraint reads min_start_minute or priority.
            .def_readwrite("min_start_minute", &Job::minStartTime)
            .def_readwrite("ideal_end_minute", &Job::idealEndTime)
            .def_readwrite("max_end_minute", &Job::maxEndTime)
            .def_readwrite("priority", &Job::priority)
            .def_readwrite("pinned", &Job::pinned)
            // Derived fields (results; refreshed by update_shadow_variables /
            // calculate_score / Solver.solve).
            .def_readonly("line_index", &Job::lineIndex)
            .def_property_readonly("start_cleaning_minute",
                                   [](const Job& j) { return timeToPy(j.startCleaningDateTime); })
            .def_property_readonly("start_production_minute",
                                   [](const Job& j) { return timeToPy(j.startProductionDateTime); })
            .def_property_readonly("end_minute",
                                   [](const Job& j) { return timeToPy(j.endDateTime); });

    py::class_<Line>(m, "Line", py::module_local())
            .def(py::init([](std::string id, std::string name, std::int64_t startMinute,
                             int operator_index, std::vector<int> jobs) {
                     Line l;
                     l.id = std::move(id);
                     l.name = std::move(name);
                     l.startDateTime = startMinute;
                     l.operator_index = operator_index;
                     l.jobs = std::move(jobs);
                     return l;
                 }),
                 py::arg("id"), py::arg("name"), py::arg("start_minute"),
                 py::arg("operator_index") = -1, py::arg("jobs") = std::vector<int>{})
            .def_readwrite("id", &Line::id)
            .def_readwrite("name", &Line::name)
            .def_readwrite("start_minute", &Line::startDateTime)
            .def_readwrite("operator_index", &Line::operator_index)
            // The decision variable: ordered job indices.
            .def_readwrite("jobs", &Line::jobs);

    py::class_<PackagingSchedule>(m, "PackagingSchedule", py::module_local())
            .def(py::init([](std::int64_t fromMinute, std::int64_t toMinute,
                             std::vector<Product> products, std::vector<Minutes> cleanupMinutes,
                             std::vector<Operator> operators, std::vector<Line> lines,
                             std::vector<Job> jobs, std::vector<int> unassignedJobs) {
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
                 py::arg("from_minute"), py::arg("to_minute"), py::arg("products"),
                 py::arg("cleanup_minutes"), py::arg("operators"), py::arg("lines"),
                 py::arg("jobs"), py::arg("unassigned_jobs") = std::vector<int>{})
            .def_readwrite("from_minute", &PackagingSchedule::fromMinute)
            .def_readwrite("to_minute", &PackagingSchedule::toMinute)
            .def_readwrite("products", &PackagingSchedule::products)
            // Flat products x products matrix indexed [previous * count +
            // current]; -1 == missing entry (throws on first use).
            .def_readwrite("cleanup_minutes", &PackagingSchedule::cleanupMinutes)
            .def_readwrite("operators", &PackagingSchedule::operators)
            .def_readwrite("lines", &PackagingSchedule::lines)
            .def_readwrite("jobs", &PackagingSchedule::jobs)
            // The unassigned pool (jobs deliberately left on no line). Jobs in
            // no list at all are placed by the construction heuristic.
            .def_readwrite("unassigned_jobs", &PackagingSchedule::unassignedJobs)
            .def_readonly("score", &PackagingSchedule::score)
            .def("line_count", &PackagingSchedule::lineCount)
            .def(
                    "cleanup_of",
                    [](const PackagingSchedule& s, int previousProduct, int product) {
                        return s.cleanup(previousProduct, product);
                    },
                    py::arg("previous_product"), py::arg("product"),
                    "Cleanup minutes when `product` follows `previous_product`; -1 "
                    "== missing entry.")
            .def("update_shadow_variables", &ScoreCalculator::updateShadowVariables,
                 "Recompute every job's derived fields (line_index, "
                 "start_cleaning/start_production/end) from the current lists.");

    m.def("calculate_score", &ScoreCalculator::score, py::arg("schedule"),
          "Full recompute of the hard/medium/soft score (also refreshes the "
          "derived job fields and stores schedule.score).");

    m.def("assign_missing_operators", &assignMissingOperators, py::arg("schedule"),
          "Give every operator-less line the least-used operator (the "
          "construction step for Line.operator_index).");

    m.def("improve_operator_assignment", &improveOperatorAssignment, py::arg("schedule"),
          "Hill-climb the line->operator assignment with the job lists "
          "fixed; returns whether anything changed.");

    m.def("generate_demo_data", &generateDemoData, py::arg("line_count") = 5,
          py::arg("job_count") = 100,
          "The demo plant (deterministic, seed 37); minute 0 is the demo's "
          "Monday 00:00.");

    py::class_<Solver::Config> config(m, "SolverConfig", py::module_local());

    optimaru::python::bind_solver_config(config);
    config.def(py::init<>())
            // 0 = auto: 1 without operator sharing, else 5 alternating rounds.
            .def_readwrite("operator_rounds", &Solver::Config::operator_rounds)
            .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
            .def_readwrite("ruin_recreate_unimproved_step_trigger",
                           &Solver::Config::ruin_recreate_unimproved_step_trigger)
            .def_readwrite("ruin_recreate_min_ruin_fraction",
                           &Solver::Config::ruin_recreate_min_ruin_fraction)
            .def_readwrite("ruin_recreate_max_ruin_fraction",
                           &Solver::Config::ruin_recreate_max_ruin_fraction)
            .def_readwrite("ruin_recreate_max_ruin_count",
                           &Solver::Config::ruin_recreate_max_ruin_count)
            .def_readwrite("ruin_recreate_operator_decay",
                           &Solver::Config::ruin_recreate_operator_decay)
            .def_readwrite("ruin_recreate_polish_steps",
                           &Solver::Config::ruin_recreate_polish_steps)
            .def_readwrite("relocate_move_weight", &Solver::Config::relocate_weight)
            .def_readwrite("list_swap_move_weight", &Solver::Config::list_swap_weight)
            .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
            .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
            .def_readwrite("tail_swap_move_weight", &Solver::Config::tail_swap_weight)
            // Nearby selection over the cleanup matrix (cheap-changeover
            // neighborhoods); probability 0 disables.
            .def_readwrite("nearby_probability", &Solver::Config::nearby_probability)
            .def_readwrite("nearby_size", &Solver::Config::nearby_size)
            // Early termination once bestScore >= (hard, medium*1e10 + soft) as
            // the ENGINE score; (0, -inf) stops at the first hard-feasible
            // solution.
            .def_property(
                    "best_score_limit",
                    [](const Solver::Config& c) {
                        return py::make_tuple(c.target_score.hard, c.target_score.soft);
                    },
                    [](Solver::Config& c, std::pair<std::int64_t, double> hs) {
                        c.target_score = optimaru::Score{hs.first, hs.second};
                    })
            // Callback fired on every new best solution: on_new_best(step,
            // score) with the hard/medium/soft score in the same levels as the
            // final schedule.score (solve_with_constraints reads them off the
            // calculator before the medium fold). The pybind11 wrapper
            // re-acquires the GIL, so it is safe from the engine's multi-start
            // worker threads too.
            .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: mutates lines[*].jobs / unassigned_jobs (and
            // lines[*].operator_index when sharing forces alternation), then
            // refreshes the derived job fields and schedule.score. The GIL is released so
            // parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<PackagingSchedule&>(&Solver::solve),
                 py::arg("schedule"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.food_packaging.constraints). The picks replace the defaults; None
            // keeps them. A name may be picked several times: every pick is its
            // own instance (a fresh copy of the default component) with its own
            // knobs. schedule.score then reports the three levels the assembly
            // put the terms on (a medium constraint with level='soft' lands on
            // soft).
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, PackagingSchedule& schedule, const py::object& constraints) {
                        ScoreCalculator::prepare(schedule);
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(schedule, calculator);
                    },
                    py::arg("schedule"), py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `schedule` — the input
    // optimaru.food_packaging.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // Units are the lists (the packaging lines in order, then the unassigned
    // pool); a global constraint has one entry, and a per-line constraint
    // lists the lines it evaluates as "units" (never the pool). Every entry
    // carries the domain's medium fold (into_soft, 10^10), which score()
    // applies. None = the default assembly; a pick list scores exactly those
    // picks (apply_constraints), each entry carrying its pick's knobs, the
    // k-th pick of a name keyed "<name>#<k>". This is the only
    // per-constraint surface (ConstraintBreakdown is not bound).
    m.def(
            "constraint_costs",
            [](PackagingSchedule& schedule, const py::object& constraints) {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                return opy::constraint_costs(calculator, schedule);
            },
            py::arg("schedule"), py::arg("constraints") = py::none(),
            "Raw per-unit cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order
    // (the unit constraints first, then the globals
    // operator_cleaning_conflict_cost and unassigned_jobs_cost) — the same
    // strings constraint_costs() reports.
    m.def(
            "list_constraints", [] { return opy::constraint_names(defaultConstraints()); },
            "Constraint names available to optimaru.food_packaging.constraints.");

    // The constraint catalog, read off the default assembly: the components'
    // kDefaultWeight / kDefaultLevel are the only place a default lives.
    opy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
