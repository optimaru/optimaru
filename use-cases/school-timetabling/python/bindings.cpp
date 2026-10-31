// pybind11 bindings for the school-timetabling C++ domain wrappers.
// The Python extension links the existing st::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here. Two domains are exposed:
//   - the quickstart port (st::Timetable, slotRoom composite encoding) at the
//     module top level, unchanged for backwards compatibility;
//   - the canonical full-parity domain (st::canon) in the `canon` submodule,
//     mirroring the CP-SAT reference package's constraints under the same
//     names (unassigned_lessons_cost ... parallel_groups_cost). Its catalog
//     is the package's: constraint_info() / ConstraintInfo at the top level.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::set / std::map / std::string conversions

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "st/canonical_constraints.hpp"
#include "st/canonical_domain.hpp"
#include "st/canonical_score_calculator.hpp"
#include "st/canonical_solver.hpp"
#include "st/domain.hpp"
#include "st/objective_weights.hpp"
#include "st/score_calculator.hpp"
#include "st/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
namespace tfpy = optimaru::python;
using namespace st;
namespace cn = st::canon;

namespace
{

// The quickstart and canonical Solver::Config structs share every engine
// field, so both bindings come from this one helper; the mode-specific
// fields (the canonical fast-path `weights` and guided-search knobs, the
// quickstart same-group nearby knobs) are bound next to each class below.
template <typename Config> void bindSolverConfigCommon(py::class_<Config> &cls)
{
    cls.def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Config &c)
            { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Config &c, double s)
            {
                c.spentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Config &c) {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "unimproved_spent_limit_seconds must be non-negative");
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit",
                       &Config::unimprovedStepCountLimit)
        .def_readwrite("random_seed", &Config::randomSeed)
        .def_property(
            "parallel_solver_count",
            [](const Config &c) { return c.parallelSolverCount; },
            [](Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error(
                        "parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // 0 = auto: the solver scales the history with the instance size
        // (quickstart clamp(8 * lessons, 400, 3200), canonical clamp(4 *
        // lessons, 400, 3200)).
        .def_property(
            "late_acceptance_size",
            [](const Config &c) { return c.lateAcceptanceSize; },
            [](Config &c, std::int64_t size)
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
            [](const Config &c) {
                return c.environmentMode
                       == optimaru::EnvironmentMode::FULL_ASSERT;
            },
            [](Config &c, bool on)
            {
                c.environmentMode = on ? optimaru::EnvironmentMode::FULL_ASSERT
                                       : optimaru::EnvironmentMode::NON_ASSERT;
            })
        .def_readwrite("ruin_recreate", &Config::ruinRecreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger",
                       &Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction",
                       &Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction",
                       &Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count",
                       &Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_operator_decay",
                       &Config::ruinRecreateOperatorDecay)
        .def_property(
            "ruin_recreate_rewards",
            [](const Config &c) { return c.ruinRecreateRewards; },
            [](Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error(
                        "ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps",
                       &Config::ruinRecreatePolishSteps)
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("descent_weight", &Config::descentWeight)
        .def_readwrite("descent_max_sweeps", &Config::descentMaxSweeps)
        .def_readwrite("change_move_weight", &Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Config::swapMoveWeight)
        // Early termination once bestScore >= (hard, soft), e.g. (0, -1e18)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Config &c) {
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.soft);
            },
            [](Config &c, std::pair<std::int64_t, double> hs)
            { c.bestScoreLimit = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Config::onNewBest);
}

// ---------------------------------------------------------------------------
// Canonical constraint picks -> the standard resolve path
// ---------------------------------------------------------------------------
// The Python names are pythonName() of the components' registry names
// (teacherGapCost -> teacher_gap_cost), resolved against the default
// assembly by tfpy::applyConstraints — no name table of our own. The one
// domain-specific piece is the TeacherMaskHub: the five teacher-mask
// components of ONE calculator share one projection, so a pick added to a
// calculator that already holds family members joins their hub
// (cn::sharedTeacherMaskHub) — the factory is defaultConstraints(hub).

// The constraints `picks` names (None = the whole default set), with their
// knobs applied, poured into `calc`.
void addPicks(cn::DecomposedCalculator &calc, const py::object &picks)
{
    tfpy::applyConstraints(
        cn::defaultConstraints(cn::sharedTeacherMaskHub(calc)), picks)
        .into(calc);
}

// The catalog order (the default set's emplace order) as Python names.
std::vector<std::string> catalogNames()
{
    std::vector<std::string> names;
    for (const std::string &name : cn::constraintOrder())
        names.push_back(tfpy::pythonName(name));
    return names;
}

// constraint_info() rows in the catalog order (ConstraintAssembly::info()
// lists the unit constraints before the global ones).
std::vector<optimaru::ConstraintInfo> catalogRows()
{
    const std::vector<optimaru::ConstraintInfo> rows
        = cn::defaultConstraints().info();
    std::vector<optimaru::ConstraintInfo> ordered;
    for (const std::string &name : cn::constraintOrder())
    {
        for (const optimaru::ConstraintInfo &row : rows)
        {
            if (row.name == name)
                ordered.push_back(row);
        }
    }
    return ordered;
}

// tfpy::constraintCosts(calc, table) with its entries in the catalog order.
py::dict orderedConstraintCosts(cn::DecomposedCalculator &calc,
                                cn::Timetable &table)
{
    cn::ScoreCalculator::prepare(table);
    const py::dict costs = tfpy::constraintCosts(calc, table);
    py::dict ordered;
    for (const std::string &name : catalogNames())
    {
        const py::str key(name);
        if (costs.contains(key))
            ordered[key] = costs[key];
    }
    for (const auto &item : costs)  // anything outside the catalog order
    {
        if (!ordered.contains(item.first))
            ordered[item.first] = item.second;
    }
    return ordered;
}

// The canonical domain keeps allowedSlot as std::vector<char> (a compact
// per-slot permission table); expose it as a Python list of bools.
std::vector<bool> allowedSlotToPython(const cn::Lesson &lesson)
{
    return {lesson.allowedSlot.begin(), lesson.allowedSlot.end()};
}

void allowedSlotFromPython(cn::Lesson &lesson, const std::vector<bool> &table)
{
    lesson.allowedSlot.assign(table.begin(), table.end());
}

// The fields of canon.ObjectiveWeights, by Python name.
struct WeightField
{
    const char *name;
    double cn::ObjectiveWeights::*member;
};

constexpr WeightField kWeightFields[] = {
    {"teacher_gap_cost", &cn::ObjectiveWeights::teacherGapCost},
    {"teacher_workday_cost", &cn::ObjectiveWeights::teacherWorkdayCost},
    {"teacher_free_day_cost", &cn::ObjectiveWeights::teacherFreeDayCost},
    {"class_gap_cost", &cn::ObjectiveWeights::classGapCost},
    {"avoided_period_cost", &cn::ObjectiveWeights::avoidedPeriodCost},
    {"non_preferred_period_cost",
     &cn::ObjectiveWeights::nonPreferredPeriodCost},
};

// One fast-path soft weight (an ObjectiveWeights argument or field), checked
// like a constraint pick's weight knob and in the same words: a real number
// (not a bool), finite and >= 0.
double checkedWeight(const char *field, const py::object &value)
{
    const std::string label
        = std::string("ObjectiveWeights.") + field + ": weight must be ";
    const std::string shown = py::repr(value).cast<std::string>();
    if (py::isinstance<py::bool_>(value)
        || !py::isinstance(value,
                           py::module_::import("numbers").attr("Real")))
        throw py::type_error(label + "a number, not " + shown);
    const double weight = py::float_(value).cast<double>();
    if (!std::isfinite(weight) || weight < 0.0)
        throw py::value_error(label + "finite and >= 0, not " + shown);
    return weight;
}

void bindCanon(py::module_ &canon)
{
    // The soft weights of the monolithic fast path (SolverConfig.weights),
    // named like the soft constraints; the defaults are the components'
    // kDefaultWeight. Never part of the model, never read by the plugin path.
    const cn::ObjectiveWeights defaultWeights{};
    py::class_<cn::ObjectiveWeights> objectiveWeights(
        canon,
        "ObjectiveWeights",
        py::module_local(),
        "Soft weights of the monolithic fast path (solve(...) without "
        "constraints=), one per soft constraint; the defaults are the C++ "
        "components' kDefaultWeight.");
    objectiveWeights
        .def(py::init(
                 [](const py::object &teacherGapCost,
                    const py::object &teacherWorkdayCost,
                    const py::object &teacherFreeDayCost,
                    const py::object &classGapCost,
                    const py::object &avoidedPeriodCost,
                    const py::object &nonPreferredPeriodCost)
                 {
                     cn::ObjectiveWeights w;
                     w.teacherGapCost
                         = checkedWeight("teacher_gap_cost", teacherGapCost);
                     w.teacherWorkdayCost = checkedWeight(
                         "teacher_workday_cost", teacherWorkdayCost);
                     w.teacherFreeDayCost = checkedWeight(
                         "teacher_free_day_cost", teacherFreeDayCost);
                     w.classGapCost
                         = checkedWeight("class_gap_cost", classGapCost);
                     w.avoidedPeriodCost = checkedWeight(
                         "avoided_period_cost", avoidedPeriodCost);
                     w.nonPreferredPeriodCost
                         = checkedWeight("non_preferred_period_cost",
                                         nonPreferredPeriodCost);
                     return w;
                 }),
             py::arg("teacher_gap_cost") = defaultWeights.teacherGapCost,
             py::arg("teacher_workday_cost")
             = defaultWeights.teacherWorkdayCost,
             py::arg("teacher_free_day_cost")
             = defaultWeights.teacherFreeDayCost,
             py::arg("class_gap_cost") = defaultWeights.classGapCost,
             py::arg("avoided_period_cost") = defaultWeights.avoidedPeriodCost,
             py::arg("non_preferred_period_cost")
             = defaultWeights.nonPreferredPeriodCost)
        .def("__eq__",
             [](const cn::ObjectiveWeights &a, const cn::ObjectiveWeights &b)
             {
                 return a.teacherGapCost == b.teacherGapCost
                        && a.teacherWorkdayCost == b.teacherWorkdayCost
                        && a.teacherFreeDayCost == b.teacherFreeDayCost
                        && a.classGapCost == b.classGapCost
                        && a.avoidedPeriodCost == b.avoidedPeriodCost
                        && a.nonPreferredPeriodCost == b.nonPreferredPeriodCost;
             })
        .def(
            "__repr__",
            [](const cn::ObjectiveWeights &w)
            {
                return py::str(
                           "ObjectiveWeights(teacher_gap_cost={}, "
                           "teacher_workday_cost={}, teacher_free_day_cost={}, "
                           "class_gap_cost={}, avoided_period_cost={}, "
                           "non_preferred_period_cost={})")
                    .format(w.teacherGapCost,
                            w.teacherWorkdayCost,
                            w.teacherFreeDayCost,
                            w.classGapCost,
                            w.avoidedPeriodCost,
                            w.nonPreferredPeriodCost);
            });
    // The fields reject what the constructor rejects.
    for (const WeightField &field : kWeightFields)
    {
        objectiveWeights.def_property(
            field.name,
            [member = field.member](const cn::ObjectiveWeights &w)
            { return w.*member; },
            [member = field.member, name = field.name](
                cn::ObjectiveWeights &w, const py::object &value)
            { w.*member = checkedWeight(name, value); });
    }

    py::class_<cn::Lesson>(canon, "Lesson", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int classGroupId,
                    int courseKey,
                    std::vector<int> teachers,
                    std::vector<int> roomTypeDemand,
                    int blockGroup,
                    bool pinned,
                    int slot)
                 {
                     cn::Lesson lesson;
                     lesson.id = std::move(id);
                     lesson.classGroupId = classGroupId;
                     lesson.courseKey = courseKey;
                     lesson.teachers = std::move(teachers);
                     lesson.roomTypeDemand = std::move(roomTypeDemand);
                     lesson.blockGroup = blockGroup;
                     lesson.pinned = pinned;
                     lesson.slot = slot;
                     return lesson;
                 }),
             py::arg("id"),
             py::arg("class_group_id"),
             py::arg("course_key"),
             py::arg("teachers"),
             py::arg("room_type_demand") = std::vector<int>{},
             py::arg("block_group") = -1,
             py::arg("pinned") = false,
             py::arg("slot") = -1)
        .def_readwrite("id", &cn::Lesson::id)
        .def_readwrite("class_group_id", &cn::Lesson::classGroupId)
        .def_readwrite("course_key", &cn::Lesson::courseKey)
        .def_readwrite("teachers", &cn::Lesson::teachers)
        .def_readwrite("room_type_demand", &cn::Lesson::roomTypeDemand)
        // Per-slot permission table (disallowed_slot_cost); empty = every
        // slot allowed.
        .def_property(
            "allowed_slot", &allowedSlotToPython, &allowedSlotFromPython)
        // Preference units (avoided_period_cost / non_preferred_period_cost):
        // one (avoided_mask, preferred_mask) pair per member
        // course of a merged lesson; empty = derive from the courseKey masks.
        .def_readwrite("preference_units", &cn::Lesson::preferenceUnits)
        .def_readwrite("block_group", &cn::Lesson::blockGroup)
        .def_readwrite("pinned", &cn::Lesson::pinned)
        .def_readwrite("slot", &cn::Lesson::slot);

    py::class_<cn::Timetable>(canon, "Timetable", py::module_local())
        .def(py::init(
                 [](std::vector<int> dayOfSlot,
                    std::vector<int> periodOfSlot,
                    std::vector<cn::Lesson> lessons)
                 {
                     cn::Timetable t;
                     t.dayOfSlot = std::move(dayOfSlot);
                     t.periodOfSlot = std::move(periodOfSlot);
                     t.lessons = std::move(lessons);
                     return t;
                 }),
             py::arg("day_of_slot"),
             py::arg("period_of_slot"),
             py::arg("lessons"))
        .def_readwrite("day_of_slot", &cn::Timetable::dayOfSlot)
        .def_readwrite("period_of_slot", &cn::Timetable::periodOfSlot)
        // NOTE: element access returns copies (like the quickstart lessons);
        // use set_assignment for bulk slot writes.
        .def_readwrite("lessons", &cn::Timetable::lessons)
        .def_readwrite("teacher_count", &cn::Timetable::teacherCount)
        .def_readwrite("class_count", &cn::Timetable::classCount)
        .def_readwrite("room_capacity_by_type",
                       &cn::Timetable::roomCapacityByType)
        .def_readwrite("teacher_max_per_day", &cn::Timetable::teacherMaxPerDay)
        .def_readwrite("teacher_max_per_week",
                       &cn::Timetable::teacherMaxPerWeek)
        .def_readwrite("teacher_max_consecutive",
                       &cn::Timetable::teacherMaxConsecutive)
        // Per-teacher preferred-free-day bitmask (bit d = 0-based day).
        .def_readwrite("teacher_preferred_free_days",
                       &cn::Timetable::teacherPreferredFreeDays)
        .def_readwrite("course_daily_limit", &cn::Timetable::courseDailyLimit)
        // Per-courseKey period masks (bit = period - 1), the default
        // preference unit for lessons without explicit preference_units.
        .def_readwrite("course_avoided_period_mask",
                       &cn::Timetable::courseAvoidedPeriodMask)
        .def_readwrite("course_preferred_period_mask",
                       &cn::Timetable::coursePreferredPeriodMask)
        .def_readonly("score", &cn::Timetable::score)
        .def_readonly("prepared", &cn::Timetable::prepared)
        .def("slot_count", &cn::Timetable::slotCount)
        // Convenience: the current slot per lesson (the solution).
        .def("assignment",
             [](const cn::Timetable &t)
             {
                 std::vector<int> a;
                 a.reserve(t.lessons.size());
                 for (const cn::Lesson &lesson : t.lessons)
                     a.push_back(lesson.slot);
                 return a;
             })
        // Bulk write of the planning variable (lesson access via .lessons
        // returns copies, so per-lesson writes would be lost).
        .def(
            "set_assignment",
            [](cn::Timetable &t, const std::vector<int> &a)
            {
                if (a.size() != t.lessons.size())
                    throw py::value_error(
                        "assignment length must equal the lesson count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    t.lessons[i].slot = a[i];
            },
            py::arg("assignment"));

    canon.def("calculate_score",
              py::overload_cast<cn::Timetable &>(
                  &cn::ScoreCalculator::calculateScore),
              py::arg("table"),
              "Full recompute of the HardSoftScore at the default weights "
              "(also stores table.score).");
    canon.def("calculate_score",
              py::overload_cast<cn::Timetable &, const cn::ObjectiveWeights &>(
                  &cn::ScoreCalculator::calculateScore),
              py::arg("table"),
              py::arg("weights"),
              "Full recompute with the soft terms at `weights` (the fast "
              "path's ObjectiveWeights; also stores table.score).");
    canon.def("prepare",
              &cn::ScoreCalculator::prepare,
              py::arg("table"),
              "Normalize the instance in place (idempotent; also run by "
              "calculate_score / constraint_costs / Solver.solve).");

    // ------------------------------------------------------------------
    // Plugin-style cost assembly: pick canonical constraints and their knobs
    // from Python, then pass the assembly to Solver.solve(table, calc).
    // ------------------------------------------------------------------
    py::class_<cn::DecomposedCalculator>(
        canon,
        "DecomposedCalculator",
        py::module_local(),
        "Pluggable score calculator: assemble canonical constraints — either "
        "from constraint picks (see st_timetabling.constraints) or one by one "
        "via add(name, ...) — then pass to Solver.solve(table, calc). The "
        "default set (default_calculator) reproduces canon.calculate_score "
        "exactly.")
        .def(py::init<>())
        // Example: DecomposedCalculator([TeacherGapCost(weight=40), ...]).
        // Duck-typed picks (.name/.weight/.hard/.exponent), resolved and
        // checked by tfpy::applyConstraints: weight None keeps the
        // component's kDefaultWeight, hard None its kDefaultLevel.
        .def(py::init(
                 [](const py::object &constraints)
                 {
                     auto calc = std::make_unique<cn::DecomposedCalculator>();
                     addPicks(*calc, constraints);
                     return calc;
                 }),
             py::arg("constraints"),
             "Assemble from constraint picks (duck-typed: .name, .weight, "
             ".hard, .exponent; None = the whole default set). The picks "
             "replace the defaults; a name picked twice or an empty list is "
             "a ValueError.")
        .def(
            "add",
            [](cn::DecomposedCalculator &calc,
               const std::string &name,
               const py::object &weight,
               const py::object &hard,
               const py::object &exponent)
            {
                const py::object pick = py::module_::import("types").attr(
                    "SimpleNamespace")(py::arg("name") = name,
                                       py::arg("weight") = weight,
                                       py::arg("hard") = hard,
                                       py::arg("exponent") = exponent);
                py::list picks;
                picks.append(pick);
                addPicks(calc, picks);
            },
            py::arg("name"),
            py::arg("weight") = py::none(),
            py::arg("hard") = py::none(),
            py::arg("exponent") = 1.0,
            "Add one canonical constraint by name (see CONSTRAINT_NAMES), "
            "with the same three knobs and checks as a spec:\n"
            "weight:   None = the component's default weight, else a finite "
            "number >= 0.\n"
            "hard:     None = the default level, True = hard, False = soft "
            "(fixed_slots_cost / parallel_groups_cost refuse False).\n"
            "exponent: sign-preserving power shaping, > 0.\n"
            "Raises ValueError for an unknown name. Adding a name twice is "
            "allowed here (their costs add up).")
        .def(
            "score",
            [](cn::DecomposedCalculator &calc, cn::Timetable &table)
            {
                cn::ScoreCalculator::prepare(table);
                calc.resetWorkingSolution(table);
                const Score s = calc.calculateScore();
                table.score = s;
                return s;
            },
            py::arg("table"),
            "Score `table`'s current assignment with this assembly (also "
            "stores table.score) and prime the calculator so "
            "constraint_totals() explains the result.")
        .def(
            "constraint_totals",
            [](const cn::DecomposedCalculator &calc)
            {
                // In the catalog order, like constraint_costs():
                // constraintTotals() lists the unit constraints before the
                // global ones. Anything outside the catalog comes last.
                const std::vector<std::string> order = cn::constraintOrder();
                const auto rank = [&order](const std::string &name)
                {
                    return std::find(order.begin(), order.end(), name)
                           - order.begin();
                };
                std::vector<optimaru::ConstraintTotal> totals
                    = calc.constraintTotals();
                std::stable_sort(totals.begin(),
                                 totals.end(),
                                 [&rank](const optimaru::ConstraintTotal &a,
                                         const optimaru::ConstraintTotal &b)
                                 { return rank(a.name) < rank(b.name); });
                py::list out;
                for (const optimaru::ConstraintTotal &t : totals)
                {
                    py::dict d;
                    d["name"] = tfpy::pythonName(t.name);
                    d["label"] = t.label;
                    d["cost"] = t.cost;  // weighted; positive = penalizing
                    d["level"] = optimaru::levelName(t.level);
                    d["is_hard"] = t.isHard;
                    out.append(d);
                }
                return out;
            },
            "Weighted cost per constraint for the last scored/solved "
            "solution (positive = penalizing): {name, label, cost, level, "
            "is_hard}, in list_constraints() order.")
        .def(
            "constraint_costs",
            [](cn::DecomposedCalculator &calc, cn::Timetable &table)
            { return orderedConstraintCosts(calc, table); },
            py::arg("table"),
            "Raw per-unit costs of this assembly's constraints on `table` "
            "(the constraint_costs() dict shape; re-primes the calculator).")
        .def_property_readonly("delta_group_count",
                               &cn::DecomposedCalculator::deltaGroupCount,
                               "Number of structural delta groups compiled by "
                               "the last reset/solve.");

    canon.attr("CONSTRAINT_NAMES") = py::tuple(py::cast(catalogNames()));

    // The documented weight of unassigned_lessons_cost demoted to soft
    // (partial timetabling): UnassignedLessonsCost::kSoftWeight.
    canon.attr("UNASSIGNED_LESSONS_SOFT_WEIGHT")
        = cn::UnassignedLessonsCost::kSoftWeight;

    canon.def(
        "default_calculator",
        []
        {
            auto calc = std::make_unique<cn::DecomposedCalculator>();
            cn::addDefaultConstraints(*calc);
            return calc;
        },
        "A DecomposedCalculator preloaded with the default set, every "
        "component at its default weight and level. Scores bit-for-bit "
        "like canon.calculate_score.");

    // The raw per-unit costs of the default set (or of `constraints`, the
    // same picks as solve(constraints=...)), before weight / exponent /
    // level — what st_timetabling.constraints.score() turns into a score.
    canon.def(
        "constraint_costs",
        [](cn::Timetable &table, const py::object &constraints)
        {
            cn::DecomposedCalculator calc;
            addPicks(calc, constraints);
            return orderedConstraintCosts(calc, table);
        },
        py::arg("table"),
        py::arg("constraints") = py::none(),
        "Raw per-unit cost of every default constraint (or of the picked "
        "ones), before weight/exponent/level: {name: {costs, weight, "
        "exponent, hard, global, level, reward, level_flippable, name, "
        "label}}, in list_constraints() order.");

    py::class_<cn::Solver::Config> config(
        canon, "SolverConfig", py::module_local());
    bindSolverConfigCommon(config);
    // The fast path's soft weights (solve(table) only; the model carries
    // none, the plugin path's components carry their own).
    config.def_readwrite("weights", &cn::Solver::Config::weights);
    // Same-class nearby swap bias (see canonical_solver.hpp).
    config.def_readwrite("same_class_nearby",
                         &cn::Solver::Config::sameClassNearby);
    config.def_readwrite("same_class_nearby_probability",
                         &cn::Solver::Config::sameClassNearbyProbability);
    // Violation-guided entity selection (see canonical_solver.hpp).
    config.def_readwrite("hot_lesson_probability",
                         &cn::Solver::Config::hotLessonProbability);
    // Smart same-class swap operator (see canonical_solver.hpp).
    config.def_readwrite("smart_class_swap_moves",
                         &cn::Solver::Config::smartClassSwapMoves);
    config.def_readwrite("smart_class_swap_candidates",
                         &cn::Solver::Config::smartClassSwapCandidates);
    // Best-of-k width of the re-assign move — only in play when the assembly
    // demotes unassigned_lessons_cost to soft (partial timetabling; see
    // canonical_solver.hpp for the sweep).
    config.def_readwrite("reassign_candidates",
                         &cn::Solver::Config::reassignCandidates);

    py::class_<cn::Solver>(canon, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<cn::Solver::Config>(), py::arg("config"))
        // Solves in place: sets lesson.slot and table.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             static_cast<void (cn::Solver::*)(cn::Timetable &)>(
                 &cn::Solver::solve),
             py::arg("table"),
             py::call_guard<py::gil_scoped_release>())
        // Same, but with a caller-assembled plugin calculator: the search
        // and the final table.score both use that assembly (custom weights
        // included; single-start — see canonical_solver.hpp).
        .def("solve",
             static_cast<void (cn::Solver::*)(cn::Timetable &,
                                              cn::DecomposedCalculator &)>(
                 &cn::Solver::solve),
             py::arg("table"),
             py::arg("calculator"),
             py::call_guard<py::gil_scoped_release>());
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "School timetabling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Timeslot>(m, "Timeslot", py::module_local())
        .def(py::init(
                 [](int day, int period)
                 {
                     Timeslot t;
                     t.day = day;
                     t.period = period;
                     return t;
                 }),
             py::arg("day"),
             py::arg("period"))
        .def_readwrite("day", &Timeslot::day)
        .def_readwrite("period", &Timeslot::period);

    py::class_<Room>(m, "Room", py::module_local())
        .def(py::init(
                 [](std::string name, std::string type)
                 {
                     Room r;
                     r.name = std::move(name);
                     r.type = std::move(type);
                     return r;
                 }),
             py::arg("name"),
             py::arg("type") = "")
        .def_readwrite("name", &Room::name)
        .def_readwrite("type", &Room::type);

    py::class_<Lesson>(m, "Lesson", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string subject,
                    std::string teacher,
                    std::string studentGroup)
                 {
                     Lesson l;
                     l.id = std::move(id);
                     l.subject = std::move(subject);
                     l.teacher = std::move(teacher);
                     l.studentGroup = std::move(studentGroup);
                     return l;
                 }),
             py::arg("id"),
             py::arg("subject"),
             py::arg("teacher"),
             py::arg("student_group"))
        .def_readwrite("id", &Lesson::id)
        .def_readwrite("subject", &Lesson::subject)
        .def_readwrite("teacher", &Lesson::teacher)
        .def_readwrite("student_group", &Lesson::studentGroup)
        .def_readwrite("slot_room", &Lesson::slotRoom)
        .def_readwrite("pinned", &Lesson::pinned);

    py::class_<Timetable>(m, "Timetable", py::module_local())
        .def(py::init(
                 [](std::vector<Timeslot> timeslots,
                    std::vector<Room> rooms,
                    std::vector<Lesson> lessons,
                    std::map<std::string, std::set<int>> teacherUnavailable)
                 {
                     Timetable t;
                     t.timeslots = std::move(timeslots);
                     t.rooms = std::move(rooms);
                     t.lessons = std::move(lessons);
                     t.teacherUnavailable = std::move(teacherUnavailable);
                     return t;
                 }),
             py::arg("timeslots"),
             py::arg("rooms"),
             py::arg("lessons"),
             py::arg("teacher_unavailable")
             = std::map<std::string, std::set<int>>{})
        .def_readwrite("timeslots", &Timetable::timeslots)
        .def_readwrite("rooms", &Timetable::rooms)
        .def_readwrite("lessons", &Timetable::lessons)
        .def_readwrite("teacher_unavailable", &Timetable::teacherUnavailable)
        .def_readonly("score", &Timetable::score)
        // composite slotRoom encoding helpers (see st/domain.hpp)
        .def("timeslot_count", &Timetable::timeslotCount)
        .def("room_count", &Timetable::roomCount)
        .def("slot_room_count", &Timetable::slotRoomCount)
        .def("slot_room_of",
             &Timetable::slotRoomOf,
             py::arg("timeslot_index"),
             py::arg("room_index"))
        .def("timeslot_index_of",
             &Timetable::timeslotIndexOf,
             py::arg("slot_room"))
        .def("room_index_of", &Timetable::roomIndexOf, py::arg("slot_room"))
        // Convenience: the current slotRoom per lesson (the solution).
        .def("assignment",
             [](const Timetable &t)
             {
                 std::vector<int> a;
                 a.reserve(t.lessons.size());
                 for (const Lesson &l : t.lessons)
                     a.push_back(l.slotRoom);
                 return a;
             })
        // Bulk write of the planning variable (lesson access via .lessons
        // returns copies, so per-lesson writes would be lost).
        .def(
            "set_assignment",
            [](Timetable &t, const std::vector<int> &a)
            {
                if (a.size() != t.lessons.size())
                    throw py::value_error(
                        "assignment length must equal the lesson count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    t.lessons[i].slotRoom = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("table"),
          "Full recompute of the HardSoftScore (also stores table.score).");

    py::class_<Solver::Config> config(m, "SolverConfig", py::module_local());
    bindSolverConfigCommon(config);
    // Same-group nearby swap bias (see st/solver.hpp).
    config.def_readwrite("same_group_nearby", &Solver::Config::sameGroupNearby);
    config.def_readwrite("same_group_nearby_probability",
                         &Solver::Config::sameGroupNearbyProbability);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets lesson.slot_room and table.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<Timetable &>(&Solver::solve),
             py::arg("table"),
             py::call_guard<py::gil_scoped_release>());

    // Canonical full-parity domain (st::canon): the CP-SAT reference
    // package's hard and soft constraints with validate.py semantics.
    py::module_ canonModule = m.def_submodule(
        "canon", "Canonical full-parity school-timetabling domain (st::canon)");
    bindCanon(canonModule);

    // The package's constraint catalog — the canonical domain's (the
    // quickstart model has no registry): constraint_info() ->
    // [ConstraintInfo(name, default_level, default_weight, level_flippable,
    // reward, default_enabled)], read off the C++ components'
    // kDefaultWeight / kDefaultLevel, in list_constraints() order. Bound on
    // the top-level module so ConstraintInfo pickles by reference. The rows
    // go through bindInfoRows directly (not bindConstraintInfo) only to keep
    // the catalog order, which ConstraintAssembly::info() does not.
    tfpy::detail::bindInfoRows(m, [] { return catalogRows(); });
    m.def(
        "list_constraints",
        [] { return catalogNames(); },
        "The canonical constraint names, in catalog order (the same as "
        "canon.CONSTRAINT_NAMES).");
}
