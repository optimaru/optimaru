// pybind11 bindings for the employee-scheduling C++ domain wrapper.
// The Python extension links the existing es::* implementation and imports the
// generic engine from ../../../include; the engine itself is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::set / std::string conversions

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "es/constraints.hpp"
#include "es/domain.hpp"
#include "es/score_calculator.hpp"
#include "es/solver.hpp"
#include "optimaru/optimaru.hpp"

namespace py = pybind11;
using namespace es;

namespace
{

// Typed getters for the per-constraint parameter dict (spec-class extra
// fields, e.g. MaxConsecutiveDays.value).
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

// Plugin registry: constraint name -> component. The names are the
// Python-facing identifiers; the display names are used in constraint_totals.
// `hard` overrides the constraint's default hard/soft level; `exponent` shapes the per-unit
// cost superlinearly (see shapeCost in the engine header); `params` carries
// constraint-specific options.
void addConstraintByName(DecomposedCalculator &calc,
                         const std::string &name,
                         double weight,
                         std::optional<bool> hard,
                         double exponent,
                         const py::dict &params)
{
    std::unique_ptr<EsUnitConstraint> unit;
    std::unique_ptr<EsGlobalConstraint> global;
    if (name == "required_skill")
        unit = std::make_unique<RequiredSkill>(weight);
    else if (name == "unavailable_employee")
        unit = std::make_unique<UnavailableEmployee>(weight);
    else if (name == "overlapping_shift")
        unit = std::make_unique<OverlappingShift>(weight);
    else if (name == "one_shift_per_day")
        unit = std::make_unique<MaxOneShiftPerDay>(weight);
    else if (name == "min_rest")
        unit = std::make_unique<MinimumRest>(
            weight,
            param<Minutes>(
                params, "min_gap_minutes", ScoreCalculator::MIN_REST_MINUTES));
    else if (name == "undesired_day")
        unit = std::make_unique<UndesiredDay>(
            weight, param<bool>(params, "per_day", false));
    else if (name == "desired_day")
        unit = std::make_unique<DesiredDay>(
            weight, param<bool>(params, "per_day", false));
    else if (name == "desired_shift")
        unit = std::make_unique<DesiredShift>(weight);
    else if (name == "max_consecutive_days")
        unit = std::make_unique<MaxConsecutiveDays>(
            weight, param<int>(params, "value", 6));
    else if (name == "min_consecutive_days")
        unit = std::make_unique<MinConsecutiveDays>(
            weight, param<int>(params, "value", 2));
    else if (name == "max_consecutive_off")
        unit = std::make_unique<MaxConsecutiveOff>(
            weight, param<int>(params, "value", 3));
    else if (name == "rest_after_consecutive_work")
        unit = std::make_unique<RestAfterConsecutiveWork>(
            weight,
            param<int>(params, "work_days", 6),
            param<int>(params, "rest_days", 3));
    else if (name == "max_weekly_minutes")
        unit = std::make_unique<MaxWeeklyMinutes>(
            weight,
            param<Minutes>(params, "minutes", 40 * 60),
            param<int>(params, "week_anchor", 0));
    else if (name == "min_weekly_minutes")
        unit = std::make_unique<MinWeeklyMinutes>(
            weight, param<Minutes>(params, "minutes", 8 * 60));
    else if (name == "min_weekly_rest_days")
        unit = std::make_unique<MinWeeklyRestDays>(
            weight, param<int>(params, "value", 1));
    else if (name == "assignment_cost")
        unit = std::make_unique<AssignmentCost>(
            weight,
            param<double>(params, "cost_per_hour", 0.0),
            param<double>(params, "cost_per_shift", 0.0),
            param<AssignmentCost::CostMatrix>(params, "shift_costs", {}));
    else if (name == "balance_assignments")
        global = std::make_unique<BalanceShiftAssignments>(weight);
    else if (name == "unassigned_shift")
        global = std::make_unique<UnassignedShift>(weight);
    else if (name == "balance_min_max_gap")
        global = std::make_unique<BalanceMinMaxGap>(
            weight, param<int>(params, "tolerance", 0));
    else if (name == "balance_weekly_minutes_gap")
        global = std::make_unique<BalanceWeeklyMinutesGap>(
            weight, param<int>(params, "tolerance_minutes", 120));
    else if (name == "shift_coverage")
        global = std::make_unique<ShiftCoverage>(
            weight,
            param<std::string>(params, "worker_skill", std::string()),
            param<int>(params, "min_count", 0),
            param<int>(params, "max_count", -1),
            param<std::vector<int>>(params, "worker_indices", {}),
            param<std::vector<int>>(params, "shift_indices", {}));
    else if (name == "location_run")
        unit = std::make_unique<LocationRun>(
            weight,
            param<std::string>(params, "location", std::string()),
            param<int>(params, "min_run", 0),
            param<int>(params, "max_run", -1),
            param<bool>(params, "waive_min_at_end", true));
    else if (name == "rest_after_location_run")
        unit = std::make_unique<RestAfterLocationRun>(
            weight,
            param<std::string>(params, "location", std::string()),
            param<int>(params, "rest_days", 1));
    else if (name == "location_run_gap")
        unit = std::make_unique<LocationRunGap>(
            weight,
            param<std::string>(params, "location", std::string()),
            param<int>(params, "min_gap", 0));
    else if (name == "worker_location_count")
        unit = std::make_unique<WorkerLocationCount>(
            weight,
            param<std::string>(params, "location", std::string()),
            param<std::string>(params, "worker_skill", std::string()),
            param<int>(params, "min_count", 0),
            param<int>(params, "max_count", -1));
    else if (name == "day_pattern")
        unit = std::make_unique<DayPattern>(
            weight, param<std::string>(params, "pattern", std::string()));
    else if (name == "target_days_off")
        unit = std::make_unique<TargetDaysOff>(
            weight,
            param<std::vector<int>>(params, "target_days", {}),
            param<int>(params, "min_count", 0));
    else if (name == "worker_day_count_in_range")
        unit = std::make_unique<WorkerDayCountInRange>(
            weight,
            param<int>(params, "lo", 0),
            param<int>(params, "hi", -1),
            param<int>(params, "min_count", 0),
            param<int>(params, "max_count", -1),
            param<std::string>(params, "location", std::string()));
    else if (name == "worker_pair_same_shift")
        global = std::make_unique<WorkerPairSameShift>(
            weight,
            param<int>(params, "anchor", -1),
            param<int>(params, "paired", -1),
            param<std::vector<int>>(params, "bucket_days", {}),
            param<std::vector<std::string>>(params, "bucket_locations", {}),
            param<bool>(params, "penalty", false));
    else if (name == "balance_location")
        global = std::make_unique<BalanceLocation>(
            weight,
            param<std::string>(params, "location", std::string()),
            param<std::string>(params, "worker_skill", std::string()),
            param<std::string>(params, "metric", std::string("ssd")),
            param<int>(params, "tolerance", 0),
            param<std::vector<int>>(params, "off_target_days", {}));
    else
    {
        throw py::value_error(
            "unknown constraint '" + name
            + "'; available: required_skill, unavailable_employee, "
              "overlapping_shift, one_shift_per_day, min_rest, "
              "undesired_day, desired_day, desired_shift, balance_assignments, "
              "max_consecutive_days, min_consecutive_days, "
              "max_consecutive_off, "
              "rest_after_consecutive_work, "
              "max_weekly_minutes, min_weekly_minutes, min_weekly_rest_days, "
              "assignment_cost, unassigned_shift, balance_min_max_gap, "
              "balance_weekly_minutes_gap, shift_coverage, location_run, "
              "rest_after_location_run, location_run_gap, "
              "worker_location_count, "
              "day_pattern, target_days_off, balance_location");
    }
    // Optional rule scope (see ScopedUnit): restrict this constraint to a
    // subset of workers / shifts. Indices are resolved by the caller and
    // arrive as params "worker_indices" / "shift_indices" (absent or None =
    // no restriction on that dimension).
    auto hasIdx = [&](const char *key)
    { return params.contains(key) && !params[key].is_none(); };
    const bool hasWorkerScope = hasIdx("worker_indices");
    const bool hasShiftScope = hasIdx("shift_indices");
    if (unit && (hasWorkerScope || hasShiftScope))
    {
        std::vector<int> workerIdx
            = hasWorkerScope ? params["worker_indices"].cast<std::vector<int>>()
                             : std::vector<int>{};
        std::vector<int> shiftIdx
            = hasShiftScope ? params["shift_indices"].cast<std::vector<int>>()
                            : std::vector<int>{};
        unit = std::make_unique<ScopedUnit>(std::move(unit),
                                            weight,
                                            std::move(workerIdx),
                                            std::move(shiftIdx),
                                            hasWorkerScope,
                                            hasShiftScope);
    }
    else if (global && (hasWorkerScope || hasShiftScope)
             && name != "shift_coverage")
    {
        // shift_coverage consumes worker_indices/shift_indices natively (its
        // groups derive from the shift set); other globals don't support scope.
        throw py::value_error("rule scope (worker_indices/shift_indices) is "
                              "not yet supported for "
                              "the global constraint '"
                              + name + "'");
    }

    if (unit)
    {
        if (hard)
            unit->isHard = *hard;
        unit->exponent = exponent;
        calc.add(std::move(unit));
    }
    else
    {
        if (hard)
            global->isHard = *hard;
        global->exponent = exponent;
        calc.add(std::move(global));
    }
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

    py::class_<Score>(m, "Score")
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

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
                                             Score{},
                                             false};
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
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the hard/soft score (also stores "
          "schedule.score).");

    // ------------------------------------------------------------------
    // Plugin-style cost assembly: pick constraints and weights from Python.
    // ------------------------------------------------------------------
    py::class_<DecomposedCalculator>(
        m,
        "DecomposedCalculator",
        "Pluggable score calculator: assemble constraints — either from a "
        "list of constraint specs (see es_scheduling.constraints) or "
        "incrementally via add(name, ...) — then pass to "
        "Solver.solve(schedule, calc). All eight defaults at weight 1 "
        "reproduce calculate_score exactly.")
        .def(py::init<>())
        // Example: DecomposedCalculator([RequiredSkill(2.0), ...]).
        // Duck-typed: each spec needs .name/.weight/.hard/.exponent, plus an
        // optional .params dict for constraint-specific options.
        .def(py::init(
                 [](const py::iterable &constraints)
                 {
                     auto calc = std::make_unique<DecomposedCalculator>();
                     for (py::handle spec : constraints)
                     {
                         py::dict params;
                         if (py::hasattr(spec, "params"))
                             params = spec.attr("params").cast<py::dict>();
                         addConstraintByName(
                             *calc,
                             spec.attr("name").cast<std::string>(),
                             spec.attr("weight").cast<double>(),
                             spec.attr("hard").cast<std::optional<bool>>(),
                             spec.attr("exponent").cast<double>(),
                             params);
                     }
                     return calc;
                 }),
             py::arg("constraints"),
             "Assemble from constraint spec objects, e.g.\n"
             "DecomposedCalculator([RequiredSkill(), MinimumRest(0.25, "
             "hard=False), MaxConsecutiveDays(value=6)]).")
        .def(
            "add",
            [](DecomposedCalculator &calc,
               const std::string &name,
               double weight,
               std::optional<bool> hard,
               double exponent,
               const py::kwargs &kwargs)
            {
                addConstraintByName(calc, name, weight, hard, exponent, kwargs);
            },
            py::arg("name"),
            py::arg("weight") = 1.0,
            py::arg("hard") = std::nullopt,
            py::arg("exponent") = 1.0,
            "Add a built-in constraint by name (see CONSTRAINT_NAMES).\n"
            "weight:   linear multiplier on the constraint's cost.\n"
            "hard:     True/False overrides the constraint's default hard/soft "
            "level (None keeps the default).\n"
            "exponent: shapes the per-employee cost superlinearly, sign-"
            "preserving: contribution = weight * sign(c)*|c|^exponent. "
            "2.0 = squared, 3.0 = cubed; penalizes concentrated violations.\n"
            "Extra keyword arguments become constraint-specific parameters "
            "(e.g. add('max_consecutive_days', value=6)).")
        .def(
            "score",
            [](DecomposedCalculator &calc, EmployeeSchedule &schedule)
            {
                ScoreCalculator::prepare(schedule);
                calc.resetWorkingSolution(schedule);
                Score s = calc.calculateScore();
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
                     calc.constraintTotals())
                {
                    py::dict d;
                    d["name"] = t.name;
                    d["cost"] = t.cost;  // weighted; positive = penalizing
                    d["is_hard"] = t.isHard;
                    out.append(d);
                }
                return out;
            },
            "Weighted cost per constraint for the last scored/solved solution "
            "(positive = penalizing, negative = rewarding).")
        .def_property_readonly("delta_group_count",
                               &DecomposedCalculator::deltaGroupCount,
                               "Number of structural delta groups compiled by "
                               "the last reset/solve.");

    m.attr("CONSTRAINT_NAMES") = py::make_tuple("required_skill",
                                                "unavailable_employee",
                                                "overlapping_shift",
                                                "one_shift_per_day",
                                                "min_rest",
                                                "undesired_day",
                                                "desired_day",
                                                "desired_shift",
                                                "balance_assignments",
                                                "max_consecutive_days",
                                                "min_consecutive_days",
                                                "max_consecutive_off",
                                                "rest_after_consecutive_work",
                                                "max_weekly_minutes",
                                                "min_weekly_minutes",
                                                "min_weekly_rest_days",
                                                "assignment_cost",
                                                "unassigned_shift",
                                                "balance_min_max_gap",
                                                "balance_weekly_minutes_gap",
                                                "shift_coverage",
                                                "location_run",
                                                "rest_after_location_run",
                                                "location_run_gap",
                                                "worker_location_count",
                                                "day_pattern",
                                                "target_days_off",
                                                "balance_location");

    m.def(
        "default_calculator",
        []()
        {
            auto calc = std::make_unique<DecomposedCalculator>();
            addDefaultConstraints(*calc);
            return calc;
        },
        "A DecomposedCalculator preloaded with all eight default constraints "
        "at weight 1 (equivalent to the built-in score).");

    py::class_<Solver::Config>(m, "SolverConfig")
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
        .def_property(
            "no_improvement_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Solver::Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "no_improvement_limit_seconds must be non-negative");
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::unimprovedStepCountLimit)
        .def_property(
            "no_improvement_step_count_limit",
            [](const Solver::Config &c) { return c.unimprovedStepCountLimit; },
            [](Solver::Config &c, std::int64_t steps)
            {
                if (steps < 0)
                    throw py::value_error(
                        "no_improvement_step_count_limit must be non-negative");
                c.unimprovedStepCountLimit = steps;
            })
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
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
            [](const Solver::Config &c) { return c.lateAcceptanceSize; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size <= 0)
                    throw py::value_error(
                        "late_acceptance_size must be positive");
                c.lateAcceptanceSize = static_cast<std::size_t>(size);
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
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("descent_weight", &Solver::Config::descentWeight)
        .def_readwrite("descent_max_sweeps", &Solver::Config::descentMaxSweeps)
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
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
            [](const Solver::Config &c) { return c.customMoveProbability; },
            [](Solver::Config &c, double probability)
            {
                if (probability < 0.0 || probability > 1.0)
                    throw py::value_error(
                        "custom_move_probability must be between 0 and 1");
                c.customMoveProbability = probability;
            })
        .def_readwrite("employee_ruin_moves",
                       &Solver::Config::employeeRuinMoves)
        .def_readwrite("employee_unassign_moves",
                       &Solver::Config::employeeUnassignMoves)
        .def_readwrite("relocate_move_weight",
                       &Solver::Config::relocateMoveWeight)
        .def_readwrite("list_swap_move_weight",
                       &Solver::Config::listSwapMoveWeight)
        .def_readwrite("two_opt_move_weight", &Solver::Config::twoOptMoveWeight)
        .def_readwrite("or_opt_move_weight", &Solver::Config::orOptMoveWeight)
        .def_readwrite("tail_swap_move_weight",
                       &Solver::Config::tailSwapMoveWeight)
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
        // Called with the GIL held (solve() runs on the calling thread).
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver")
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets shift.employee_index and schedule.score.
        .def("solve",
             py::overload_cast<EmployeeSchedule &>(&Solver::solve),
             py::arg("schedule"))
        // Same, but with a caller-assembled plugin calculator: the search and
        // the final schedule.score both use that assembly (custom weights).
        .def("solve",
             py::overload_cast<
                 EmployeeSchedule &,
                 optimaru::DecomposedScoreCalculator<EmployeeSchedule> &>(
                 &Solver::solve),
             py::arg("schedule"),
             py::arg("calculator"));
}
