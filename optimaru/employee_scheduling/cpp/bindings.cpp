// pybind11 bindings for the employee-scheduling C++ domain wrapper.
// The Python extension links the existing es::* implementation and imports the
// generic engine from ../../../include; the engine itself is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::set / std::string conversions

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "es/constraints.hpp"
#include "es/domain.hpp"
#include "es/score_calculator.hpp"
#include "es/solver.hpp"
#include "optimaru/optimaru.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace es;

namespace
{

namespace tfpy = optimaru::python;

// Typed getters for the per-constraint parameter dict (the spec's domain
// fields, e.g. MaxConsecutiveDaysCost.value). A missing key or None keeps
// the fallback, which is the C++ component's own constructor default.
template <typename T>
T param(const py::dict &params, const char *key, T fallback)
{
    if (!params.contains(key))
        return fallback;
    py::object v = params[key];
    if (v.is_none())
        return fallback;
    return v.cast<T>();
}

// One component the factory built, at its kDefaultWeight / kDefaultLevel:
// a unit constraint or a global one.
struct Built
{
    std::unique_ptr<EsUnitConstraint> unit;
    std::unique_ptr<EsGlobalConstraint> global;
    optimaru::ConstraintMetadata &metadata()
    {
        if (unit)
            return *unit;
        return *global;
    }
};

template <typename C, typename... Args>
Built unitOf(Args &&...args)
{
    Built built;
    built.unit = std::make_unique<C>(C::kDefaultWeight, std::forward<Args>(args)...);
    return built;
}

template <typename C, typename... Args>
Built globalOf(Args &&...args)
{
    Built built;
    built.global = std::make_unique<C>(C::kDefaultWeight, std::forward<Args>(args)...);
    return built;
}

// The rule factory: Python name -> builder, in CONSTRAINT_NAMES order. ONE
// table: CONSTRAINT_NAMES, the "available:" list of an unknown name and the
// lookup all read it, and es::catalogConstraints() holds the same components
// (tests/test_constraint_specs.py checks the two agree). Each builder passes
// the component's kDefaultWeight and reads the domain parameters after it;
// the knobs are applied afterwards (apply_knobs), on the outermost component.
struct FactoryEntry
{
    const char *name;
    Built (*build)(const py::dict &params);
};

const std::vector<FactoryEntry> &factory()
{
    static const std::vector<FactoryEntry> table = {
        {"required_skill_cost",
         [](const py::dict &) { return unitOf<RequiredSkillCost>(); }},
        {"employee_unavailability_cost",
         [](const py::dict &) { return unitOf<EmployeeUnavailabilityCost>(); }},
        {"shift_overlap_cost",
         [](const py::dict &) { return unitOf<ShiftOverlapCost>(); }},
        {"max_one_shift_per_day_cost",
         [](const py::dict &) { return unitOf<MaxOneShiftPerDayCost>(); }},
        {"min_rest_cost",
         [](const py::dict &p)
         {
             return unitOf<MinRestCost>(param<Minutes>(
                 p, "min_gap_minutes", ScoreCalculator::MIN_REST_MINUTES));
         }},
        {"undesired_day_cost",
         [](const py::dict &p)
         { return unitOf<UndesiredDayCost>(param<bool>(p, "per_day", false)); }},
        {"desired_day_cost",
         [](const py::dict &p)
         { return unitOf<DesiredDayCost>(param<bool>(p, "per_day", false)); }},
        {"desired_shift_cost",
         [](const py::dict &) { return unitOf<DesiredShiftCost>(); }},
        {"shift_count_imbalance_cost",
         [](const py::dict &) { return globalOf<ShiftCountImbalanceCost>(); }},
        {"max_consecutive_days_cost",
         [](const py::dict &p)
         { return unitOf<MaxConsecutiveDaysCost>(param<int>(p, "value", 6)); }},
        {"min_consecutive_days_cost",
         [](const py::dict &p)
         { return unitOf<MinConsecutiveDaysCost>(param<int>(p, "value", 2)); }},
        {"max_consecutive_days_off_cost",
         [](const py::dict &p)
         { return unitOf<MaxConsecutiveDaysOffCost>(param<int>(p, "value", 3)); }},
        {"rest_after_consecutive_work_cost",
         [](const py::dict &p)
         {
             return unitOf<RestAfterConsecutiveWorkCost>(
                 param<int>(p, "work_days", 6), param<int>(p, "rest_days", 3));
         }},
        {"max_weekly_minutes_cost",
         [](const py::dict &p)
         {
             return unitOf<MaxWeeklyMinutesCost>(
                 param<Minutes>(p, "minutes", 40 * 60),
                 param<int>(p, "week_anchor", 0));
         }},
        {"min_weekly_minutes_cost",
         [](const py::dict &p)
         {
             return unitOf<MinWeeklyMinutesCost>(
                 param<Minutes>(p, "minutes", 8 * 60),
                 param<int>(p, "week_anchor", 0));
         }},
        {"min_weekly_rest_days_cost",
         [](const py::dict &p)
         { return unitOf<MinWeeklyRestDaysCost>(param<int>(p, "value", 1)); }},
        {"assignment_cost",
         [](const py::dict &p)
         {
             return unitOf<AssignmentCost>(
                 param<double>(p, "cost_per_hour", 0.0),
                 param<double>(p, "cost_per_shift", 0.0),
                 param<AssignmentCost::CostMatrix>(p, "shift_costs", {}));
         }},
        {"unassigned_shifts_cost",
         [](const py::dict &) { return globalOf<UnassignedShiftsCost>(); }},
        {"shift_count_gap_cost",
         [](const py::dict &p)
         { return globalOf<ShiftCountGapCost>(param<int>(p, "tolerance", 0)); }},
        {"weekly_minutes_gap_cost",
         [](const py::dict &p)
         {
             return globalOf<WeeklyMinutesGapCost>(
                 param<int>(p, "tolerance_minutes", 120));
         }},
        {"shift_coverage_cost",
         [](const py::dict &p)
         {
             // Scoped natively: its groups derive from the shift set.
             return globalOf<ShiftCoverageCost>(
                 param<std::string>(p, "worker_skill", std::string()),
                 param<int>(p, "min_count", 0),
                 param<int>(p, "max_count", -1),
                 param<std::vector<int>>(p, "worker_indices", {}),
                 param<std::vector<int>>(p, "shift_indices", {}));
         }},
        {"location_run_length_cost",
         [](const py::dict &p)
         {
             return unitOf<LocationRunLengthCost>(
                 param<std::string>(p, "location", std::string()),
                 param<int>(p, "min_run", 0),
                 param<int>(p, "max_run", -1),
                 param<bool>(p, "waive_min_at_end", true));
         }},
        {"rest_after_location_run_cost",
         [](const py::dict &p)
         {
             return unitOf<RestAfterLocationRunCost>(
                 param<std::string>(p, "location", std::string()),
                 param<int>(p, "rest_days", 1));
         }},
        {"location_run_gap_cost",
         [](const py::dict &p)
         {
             return unitOf<LocationRunGapCost>(
                 param<std::string>(p, "location", std::string()),
                 param<int>(p, "min_gap", 0));
         }},
        {"worker_location_count_cost",
         [](const py::dict &p)
         {
             return unitOf<WorkerLocationCountCost>(
                 param<std::string>(p, "location", std::string()),
                 param<std::string>(p, "worker_skill", std::string()),
                 param<int>(p, "min_count", 0),
                 param<int>(p, "max_count", -1));
         }},
        {"day_pattern_cost",
         [](const py::dict &p)
         {
             return unitOf<DayPatternCost>(
                 param<std::string>(p, "pattern", std::string()));
         }},
        {"target_days_off_cost",
         [](const py::dict &p)
         {
             return unitOf<TargetDaysOffCost>(
                 param<std::vector<int>>(p, "target_days", {}),
                 param<int>(p, "min_count", 0));
         }},
        {"worker_day_count_in_range_cost",
         [](const py::dict &p)
         {
             return unitOf<WorkerDayCountInRangeCost>(
                 param<int>(p, "lo", 0),
                 param<int>(p, "hi", -1),
                 param<int>(p, "min_count", 0),
                 param<int>(p, "max_count", -1),
                 param<std::string>(p, "location", std::string()));
         }},
        {"location_imbalance_cost",
         [](const py::dict &p)
         {
             return globalOf<LocationImbalanceCost>(
                 param<std::string>(p, "location", std::string()),
                 param<std::string>(p, "worker_skill", std::string()),
                 param<std::string>(p, "metric", std::string("ssd")),
                 param<int>(p, "tolerance", 0),
                 param<std::vector<int>>(p, "off_target_days", {}));
         }},
        {"worker_pairing_cost",
         [](const py::dict &p)
         {
             return globalOf<WorkerPairingCost>(
                 param<int>(p, "anchor", -1),
                 param<int>(p, "paired", -1),
                 param<std::vector<int>>(p, "bucket_days", {}),
                 param<std::vector<std::string>>(p, "bucket_locations", {}),
                 param<bool>(p, "penalty", false));
         }},
    };
    return table;
}

std::vector<std::string> factoryNames()
{
    std::vector<std::string> names;
    for (const FactoryEntry &entry : factory())
        names.emplace_back(entry.name);
    return names;
}

// The domain parameters of one pick: `params()` (the shared spec base and
// the pydantic rule models), a plain `params` dict, or none at all (a
// duck-typed pick that only carries the knobs).
py::dict paramsOf(const py::handle &pick)
{
    py::object params = py::getattr(pick, "params", py::none());
    if (params.is_none())
        return py::dict();
    if (PyCallable_Check(params.ptr()))
        params = params();
    return params.cast<py::dict>();
}

// Builds one pick and adds it to `calc`: the factory component (at its
// kDefaultWeight / kDefaultLevel and the given parameters), wrapped in a
// ScopedUnit when the pick has a worker / shift scope, with the pick's
// knobs re-stated on the outermost component by tfpy::apply_knobs (weight
// None keeps kDefaultWeight, level None keeps kDefaultLevel; a reward refuses
// level='hard'; the type and range checks speak the Python base's words).
//
// The factory may add one constraint several times (other parameters, scope
// or knobs, or the same ones): each instance is a fresh component, scored on
// its own and told apart by its label (identical picks share one), and
// constraint_costs() keys the k-th instance "<name>#<k>". The knobs include
// offset / clip (readKnobs), which shape each evaluated employee's cost (a
// worker scope's employees only) or a global rule's total.
void addConstraintByName(DecomposedCalculator &calc,
                         const py::handle &pick,
                         const py::dict &params)
{
    const std::string name = pick.attr("name").cast<std::string>();
    const FactoryEntry *entry = nullptr;
    for (const FactoryEntry &candidate : factory())
        if (name == candidate.name)
            entry = &candidate;
    if (entry == nullptr)
    {
        std::string known;
        for (const FactoryEntry &candidate : factory())
            known += (known.empty() ? "" : ", ") + std::string(candidate.name);
        throw py::value_error("unknown constraint '" + name
                              + "'; available: " + known);
    }
    Built built = entry->build(params);
    if (tfpy::python_name(built.metadata().name) != name)
    {
        throw std::logic_error("factory entry '" + name + "' builds '"
                               + built.metadata().name + "'");
    }

    // Optional rule scope (see ScopedUnit): restrict this constraint to a
    // subset of workers / shifts. Indices are resolved by the caller and
    // arrive as params "worker_indices" / "shift_indices" (absent or None =
    // no restriction on that dimension).
    auto hasIdx = [&](const char *key)
    { return params.contains(key) && !params[key].is_none(); };
    const bool hasWorkerScope = hasIdx("worker_indices");
    const bool hasShiftScope = hasIdx("shift_indices");
    if (built.unit && (hasWorkerScope || hasShiftScope))
    {
        std::vector<int> workerIdx
            = hasWorkerScope ? params["worker_indices"].cast<std::vector<int>>()
                             : std::vector<int>{};
        std::vector<int> shiftIdx
            = hasShiftScope ? params["shift_indices"].cast<std::vector<int>>()
                            : std::vector<int>{};
        built.unit = std::make_unique<ScopedUnit>(std::move(built.unit),
                                                  std::move(workerIdx),
                                                  std::move(shiftIdx),
                                                  hasWorkerScope,
                                                  hasShiftScope);
    }
    else if (built.global && (hasWorkerScope || hasShiftScope)
             && name != "shift_coverage_cost")
    {
        // shift_coverage_cost consumes worker_indices/shift_indices natively
        // (its groups derive from the shift set); other globals don't
        // support scope.
        throw py::value_error("rule scope (worker_indices/shift_indices) is "
                              "not yet supported for "
                              "the global constraint '"
                              + name + "'");
    }

    tfpy::apply_knobs(built.metadata(), pick, name, optimaru::MediumFold::none());
    if (built.unit)
        calc.add(std::move(built.unit));
    else
        calc.add(std::move(built.global));
}

// A calculator holding exactly `constraints` (each pick added by the
// factory), or the default assembly for None. The selection rules of the
// Python get_constraints(): an empty list is an error. A name may be picked
// several times, with other parameters or scope or with the same ones: every
// pick is its own instance with its own knobs, scored on its own, and the
// instances add up (identical picks count twice). constraint_costs() keys
// the k-th instance of a name "<name>#<k>", in pick order.
std::unique_ptr<DecomposedCalculator> calculatorFor(const py::object &constraints)
{
    auto calc = std::make_unique<DecomposedCalculator>();
    if (constraints.is_none())
    {
        addDefaultConstraints(*calc);
        return calc;
    }
    std::size_t picked = 0;
    for (py::handle pick : constraints)
    {
        addConstraintByName(*calc, pick, paramsOf(pick));
        ++picked;
    }
    if (picked == 0)
        throw py::value_error(
            "constraints must not be empty (pass None for the defaults)");
    return calc;
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Employee shift scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    // Minutes-from-day-0 helper, matching the C++ time convention.
    m.def(
        "minutes",
        [](int day, int hour, int minute) -> Minutes
        {
            return static_cast<Minutes>(day) * MINUTES_PER_DAY + hour * 60
                   + minute;
        },
        py::arg("day"),
        py::arg("hour") = 0,
        py::arg("minute") = 0,
        "Minutes from day 0 00:00 (the time unit used by Shift.start/end).");
    m.attr("MINUTES_PER_DAY") = MINUTES_PER_DAY;
    m.attr("MAX_DAY") = MAX_DAY;

    py::class_<Score>(m, "Score")
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Employee>(m, "Employee")
        .def(py::init(
                 [](std::string name,
                    std::set<std::string> skills,
                    std::set<DayIndex> unavailable,
                    std::set<DayIndex> undesired,
                    std::set<DayIndex> desired)
                 {
                     Employee e;
                     e.name = std::move(name);
                     e.skills = std::move(skills);
                     e.unavailableDates = std::move(unavailable);
                     e.undesiredDates = std::move(undesired);
                     e.desiredDates = std::move(desired);
                     return e;
                 }),
             py::arg("name"),
             py::arg("skills") = std::set<std::string>{},
             py::arg("unavailable_dates") = std::set<DayIndex>{},
             py::arg("undesired_dates") = std::set<DayIndex>{},
             py::arg("desired_dates") = std::set<DayIndex>{})
        .def_readwrite("name", &Employee::name)
        .def_readwrite("skills", &Employee::skills)
        .def_readwrite("unavailable_dates", &Employee::unavailableDates)
        .def_readwrite("undesired_dates", &Employee::undesiredDates)
        .def_readwrite("desired_dates", &Employee::desiredDates)
        .def_readwrite("desired_shifts", &Employee::desiredShifts);

    py::class_<Shift>(m, "Shift")
        .def(py::init(
                 [](std::string id,
                    Minutes start,
                    Minutes end,
                    std::string location,
                    std::string requiredSkill)
                 {
                     Shift s;
                     s.id = std::move(id);
                     s.start = start;
                     s.end = end;
                     s.location = std::move(location);
                     s.requiredSkill = std::move(requiredSkill);
                     return s;
                 }),
             py::arg("id"),
             py::arg("start"),
             py::arg("end"),
             py::arg("location") = "",
             py::arg("required_skill") = "")
        .def_readwrite("id", &Shift::id)
        .def_readwrite("start", &Shift::start)
        .def_readwrite("end", &Shift::end)
        .def_readwrite("location", &Shift::location)
        .def_readwrite("required_skill", &Shift::requiredSkill)
        .def_readwrite("employee_index", &Shift::employeeIndex)
        .def_readwrite("pinned", &Shift::pinned);

    py::class_<EmployeeSchedule>(m, "EmployeeSchedule")
        .def(py::init(
                 [](std::vector<Employee> employees, std::vector<Shift> shifts)
                 {
                     return EmployeeSchedule{std::move(employees),
                                             std::move(shifts),
                                             Score{}};
                 }),
             py::arg("employees"),
             py::arg("shifts"))
        .def_readwrite("employees", &EmployeeSchedule::employees)
        .def_readwrite("shifts", &EmployeeSchedule::shifts)
        .def_readonly("score", &EmployeeSchedule::score)
        // Convenience: the current employee index per shift (the solution).
        .def("assignment",
             [](const EmployeeSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.shifts.size());
                 for (const Shift &sh : s.shifts)
                     a.push_back(sh.employeeIndex);
                 return a;
             });

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the hard/soft score (also stores "
          "schedule.score).");

    // ------------------------------------------------------------------
    // Plugin-style cost assembly: pick constraints and knobs from Python.
    // ------------------------------------------------------------------
    py::class_<DecomposedCalculator>(
        m,
        "DecomposedCalculator",
        "Pluggable score calculator: assemble constraints — either from a "
        "list of constraint specs (see optimaru.employee_scheduling.constraints) or "
        "incrementally via add(name, ...) — then pass to "
        "Solver.solve(schedule, calc). The eight defaults at their default "
        "weights reproduce calculate_score exactly. A constraint may be "
        "added several times (with other parameters, scope or knobs, or the "
        "same ones): each is its own instance, and the instances add up.")
        .def(py::init<>())
        // Example: DecomposedCalculator([RequiredSkillCost(weight=2.0), ...]).
        // Duck-typed: each pick needs .name/.weight/.level/.exponent (and
        // optionally .offset/.clip), plus an optional params() (or .params
        // dict) with the domain parameters. The get_constraints() rules: no
        // empty list (build an empty calculator with DecomposedCalculator()
        // and add()); a rule may be picked several times, each pick its own
        // instance.
        .def(py::init([](const py::iterable &constraints)
                      { return calculatorFor(constraints); }),
             py::arg("constraints"),
             "Assemble from constraint spec objects, e.g.\n"
             "DecomposedCalculator([RequiredSkillCost(), MinRestCost(weight=0.25, "
             "level='soft'), MaxConsecutiveDaysCost(value=6)]).")
        .def(
            "add",
            [](DecomposedCalculator &calc,
               const py::object &name,
               const py::object &weight,
               const py::object &level,
               const py::object &exponent,
               const py::object &offset,
               const py::object &clip,
               const py::kwargs &kwargs)
            {
                // kwargs are domain parameters, which the factory reads by name
                // and otherwise ignores: hard= must not vanish there.
                tfpy::refuse_hard_keyword(kwargs);
                // The same checks as a spec pick: a SimpleNamespace carrying
                // the five knobs goes through the one factory path.
                py::object pick = py::module_::import("types").attr(
                    "SimpleNamespace")(py::arg("name") = name,
                                       py::arg("weight") = weight,
                                       py::arg("level") = level,
                                       py::arg("exponent") = exponent,
                                       py::arg("offset") = offset,
                                       py::arg("clip") = clip);
                addConstraintByName(calc, pick, kwargs);
            },
            py::arg("name"),
            py::arg("weight") = py::none(),
            py::arg("level") = py::none(),
            py::arg("exponent") = 1.0,
            py::kw_only(),
            py::arg("offset") = 0.0,
            py::arg("clip") = true,
            "Add a built-in constraint by name (see CONSTRAINT_NAMES). A name "
            "may be added several times: each call adds its own instance.\n"
            "weight:   None = the component's default weight; otherwise a "
            "linear multiplier (finite, >= 0) on its cost.\n"
            "level:    None keeps the constraint's default level, otherwise "
            "'hard' or 'soft' (a reward refuses 'hard').\n"
            "exponent: shapes the per-employee cost superlinearly, sign-"
            "preserving: contribution = weight * sign(c)*|c|^exponent. "
            "2.0 = squared, 3.0 = cubed; penalizes concentrated violations.\n"
            "offset:   (keyword) shifts that shaping, any finite number; with "
            "clip=True (the default) only what lies beyond it counts, "
            "sign(c) * max(0, |c| - offset)^exponent; with clip=False the "
            "literal (c - offset)^exponent, which charges every employee the "
            "rule evaluates (a raw 0 included) and rewards a cost below the "
            "offset; with an offset it is refused on hard (ValueError), "
            "because hard 0 must mean no violation.\n"
            "Extra keyword arguments become constraint-specific parameters "
            "(e.g. add('max_consecutive_days_cost', value=6)).")
        .def(
            "score",
            [](DecomposedCalculator &calc, EmployeeSchedule &schedule)
            {
                ScoreCalculator::prepare(schedule);
                calc.rebuild(schedule);
                Score s = calc.score();
                schedule.score = s;
                return s;
            },
            py::arg("schedule"),
            "Score `schedule` with this assembly (also stores schedule.score) "
            "and "
            "prime the calculator so constraint_totals() explains the result.")
        .def(
            "constraint_totals",
            [](const DecomposedCalculator &calc)
            {
                py::list out;
                for (const optimaru::ConstraintTotal &t :
                     calc.constraint_totals())
                {
                    py::dict d;
                    d["name"] = tfpy::python_name(t.name);
                    d["label"] = t.label;  // the instance's display text
                    d["cost"] = t.cost;    // weighted; positive = penalizing
                    d["level"] = optimaru::level_name(t.level);
                    d["is_hard"] = t.is_hard;
                    out.append(d);
                }
                return out;
            },
            "Weighted cost per constraint instance for the last scored/solved "
            "solution (positive = penalizing, negative = rewarding): "
            "{name, label, cost, level, is_hard}.")
        .def_property_readonly("delta_group_count",
                               &DecomposedCalculator::delta_group_count,
                               "Number of structural delta groups compiled by "
                               "the last reset/solve.");

    // The names the rule factory accepts, in factory order: one table (see
    // factory() above) also behind the "available:" list of an unknown name.
    m.attr("CONSTRAINT_NAMES") = py::tuple(py::cast(factoryNames()));

    m.def(
        "default_calculator",
        []()
        {
            auto calc = std::make_unique<DecomposedCalculator>();
            addDefaultConstraints(*calc);
            return calc;
        },
        "A DecomposedCalculator preloaded with the eight default constraints "
        "at their default weights (equivalent to the built-in score).");

    m.def(
        "constraint_costs",
        [](EmployeeSchedule &schedule, const py::object &constraints)
        {
            ScoreCalculator::prepare(schedule);
            auto calc = calculatorFor(constraints);
            return tfpy::constraint_costs(*calc, schedule);
        },
        py::arg("schedule"),
        py::arg("constraints") = py::none(),
        "Raw per-employee cost of every constraint instance (the eight "
        "defaults for None), before weight/exponent/offset/level: {key: "
        "{costs, weight, exponent, offset, clip, hard, global, level, reward, "
        "level_flippable, name, label[, units]}}. The k-th instance of a name "
        "picked more than once is keyed '<name>#<k>'; a worker-scoped rule "
        "carries the employees it evaluates as 'units'.");

    // The constraint catalog: the eight defaults (default_enabled=True),
    // then every other constraint the factory builds (default_enabled=False),
    // read off es::defaultConstraints() / es::catalogConstraints().
    tfpy::bind_constraint_info(
        m,
        [] { return defaultConstraints(); },
        [] { return catalogConstraints(); });

    optimaru::python::bind_config_copy(py::class_<Solver::Config>(m, "SolverConfig"))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.time_limit = optimaru::python::seconds_to_millis(s, "spent_limit_seconds"); })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.stall_time_limit)
                    .count();
            },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "unimproved_spent_limit_seconds"); })
        .def_property(
            "no_improvement_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.stall_time_limit)
                    .count();
            },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "no_improvement_limit_seconds"); })
        .def_readwrite("step_count_limit", &Solver::Config::step_limit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::stall_step_limit)
        .def_property(
            "no_improvement_step_count_limit",
            [](const Solver::Config &c) { return c.stall_step_limit; },
            [](Solver::Config &c, std::int64_t steps)
            {
                if (steps < 0)
                    throw py::value_error(
                        "no_improvement_step_count_limit must be non-negative");
                c.stall_step_limit = steps;
            })
        .def_readwrite("random_seed", &Solver::Config::seed)
        .def_readwrite("guided_moves", &Solver::Config::guidedMoves)
        .def_readwrite("employee_greedy_construction",
                       &Solver::Config::employeeGreedyConstruction)
        .def_property(
            "greedy_repair_budget_fraction",
            [](const Solver::Config &c)
            { return c.greedyRepairBudgetFraction; },
            [](Solver::Config &c, double fraction)
            {
                if (fraction < 0.0 || fraction >= 0.9)
                    throw py::value_error(
                        "greedy_repair_budget_fraction must be in [0, 0.9)");
                c.greedyRepairBudgetFraction = fraction;
            })
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.history_length; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size <= 0)
                    throw py::value_error(
                        "late_acceptance_size must be positive");
                c.history_length = static_cast<std::size_t>(size);
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
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
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
        .def_readwrite("descent_max_sweeps", &Solver::Config::descent_max_sweeps)
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        .def_readwrite("nearby_shifts", &Solver::Config::nearbyShifts)
        .def_property(
            "nearby_shifts_probability",
            [](const Solver::Config &c) { return c.nearbyShiftsProbability; },
            [](Solver::Config &c, double probability)
            {
                if (probability < 0.0 || probability > 1.0)
                    throw py::value_error(
                        "nearby_shifts_probability must be between 0 and 1");
                c.nearbyShiftsProbability = probability;
            })
        .def_readwrite("smart_change_moves", &Solver::Config::smartChangeMoves)
        .def_property(
            "smart_change_candidate_count",
            [](const Solver::Config &c) { return c.smartChangeCandidateCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error(
                        "smart_change_candidate_count must be positive");
                c.smartChangeCandidateCount = count;
            })
        .def_readwrite("employee_day_repair_moves",
                       &Solver::Config::employeeDayRepairMoves)
        .def_readwrite("employee_run_repair_moves",
                       &Solver::Config::employeeRunRepairMoves)
        .def_property(
            "employee_run_repair_max_days",
            [](const Solver::Config &c) { return c.employeeRunRepairMaxDays; },
            [](Solver::Config &c, int days)
            {
                if (days <= 0)
                    throw py::value_error(
                        "employee_run_repair_max_days must be positive");
                c.employeeRunRepairMaxDays = days;
            })
        .def_readwrite("employee_window_moves",
                       &Solver::Config::employeeWindowMoves)
        .def_property(
            "employee_window_max_days",
            [](const Solver::Config &c) { return c.employeeWindowMaxDays; },
            [](Solver::Config &c, int days)
            {
                if (days <= 0)
                    throw py::value_error(
                        "employee_window_max_days must be positive");
                c.employeeWindowMaxDays = days;
            })
        .def_property(
            "custom_move_probability",
            [](const Solver::Config &c) { return c.custom_move_probability; },
            [](Solver::Config &c, double probability)
            {
                if (probability < 0.0 || probability > 1.0)
                    throw py::value_error(
                        "custom_move_probability must be between 0 and 1");
                c.custom_move_probability = probability;
            })
        .def_readwrite("employee_ruin_moves",
                       &Solver::Config::employeeRuinMoves)
        .def_readwrite("employee_unassign_moves",
                       &Solver::Config::employeeUnassignMoves)
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocate_weight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::list_swap_weight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tail_swap_weight)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.target_score.hard,
                                      c.target_score.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.target_score = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, which solve() releases.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver")
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets shift.employee_index and schedule.score. The GIL
        // is released so other Python threads keep running during the solve.
        .def("solve",
             py::overload_cast<EmployeeSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same, but with a caller-assembled plugin calculator: the search and
        // the final schedule.score both use that assembly (custom weights).
        .def("solve",
             py::overload_cast<
                 EmployeeSchedule &,
                 optimaru::DecomposedObjective<EmployeeSchedule> &>(
                 &Solver::solve),
             py::arg("schedule"),
             py::arg("calculator"),
             py::call_guard<py::gil_scoped_release>());
}
