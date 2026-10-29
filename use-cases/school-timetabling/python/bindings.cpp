// pybind11 bindings for the school-timetabling C++ domain wrappers.
// The Python extension links the existing st::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here. Two domains are exposed:
//   - the quickstart port (st::Timetable, slotRoom composite encoding) at the
//     module top level, unchanged for backwards compatibility;
//   - the canonical full-parity domain (st::canon) in the `canon` submodule,
//     mirroring the CP-SAT reference package's H1~H10 / S1~S6 semantics.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::set / std::map / std::string conversions

#include <chrono>
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
#include "st/score_calculator.hpp"
#include "st/solver.hpp"

namespace py = pybind11;
using namespace st;
namespace cn = st::canon;

namespace
{

// The quickstart and canonical Solver::Config structs share every engine
// field, so both bindings come from this one helper; the mode-specific
// fields (canonical `weights` and the guided-search knobs, the quickstart
// same-group nearby knobs) are bound next to each class below.
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
            [](const Config &c)
            {
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
            [](const Config &c)
            {
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
            [](const Config &c)
            {
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

// Plugin registry: constraint name ->
// st::canon component (see st/canonical_constraints.hpp). The names are the
// Python-facing identifiers; the S1~S6 ones match the Breakdown/penalties
// keys and the ObjectiveWeights fields. `weight` std::nullopt keeps the
// component's default (hard H* = 1, soft S* = the CP-SAT ObjectiveWeights
// default); `hard` overrides the component's default hard/soft level;
// `exponent` shapes the per-unit cost superlinearly (see shapeCost in the
// engine header).
void addCanonConstraintByName(cn::DecomposedCalculator &calc,
                              const std::string &name,
                              std::optional<double> weight,
                              std::optional<bool> hard,
                              double exponent)
{
    const cn::ObjectiveWeights defaults;
    const auto w = [&](double fallback) { return weight.value_or(fallback); };
    std::unique_ptr<cn::CanonUnitConstraint> unit;
    std::unique_ptr<cn::CanonGlobalConstraint> global;
    if (name == "unassigned_lesson")
        global = std::make_unique<cn::UnassignedLesson>(w(1.0));
    else if (name == "class_conflict")
        unit = std::make_unique<cn::ClassConflict>(w(1.0));
    else if (name == "teacher_conflict")
        unit = std::make_unique<cn::TeacherConflict>(w(1.0));
    else if (name == "slot_not_allowed")
        unit = std::make_unique<cn::SlotNotAllowed>(w(1.0));
    else if (name == "room_capacity")
        unit = std::make_unique<cn::RoomCapacity>(w(1.0));
    else if (name == "course_daily_limit")
        global = std::make_unique<cn::CourseDailyLimit>(w(1.0));
    else if (name == "block_adjacency")
        global = std::make_unique<cn::BlockAdjacency>(w(1.0));
    // The H9/H10/S1~S3 mask family shares one projection per calculator:
    // adopt the hub of an already-added family member (nullptr = the new
    // component creates its own, becoming the hub other members adopt).
    else if (name == "teacher_load_limit")
        global = std::make_unique<cn::TeacherLoadLimit>(
            w(1.0), cn::sharedTeacherMaskHub(calc));
    else if (name == "teacher_consecutive_limit")
        global = std::make_unique<cn::TeacherConsecutiveLimit>(
            w(1.0), cn::sharedTeacherMaskHub(calc));
    else if (name == "teacher_gap")
        global = std::make_unique<cn::TeacherGap>(
            w(defaults.teacherGap), cn::sharedTeacherMaskHub(calc));
    else if (name == "teacher_workday")
        global = std::make_unique<cn::TeacherWorkday>(
            w(defaults.teacherWorkday), cn::sharedTeacherMaskHub(calc));
    else if (name == "teacher_free_day")
        global = std::make_unique<cn::TeacherFreeDay>(
            w(defaults.teacherFreeDay), cn::sharedTeacherMaskHub(calc));
    else if (name == "class_gap")
        global = std::make_unique<cn::ClassGap>(w(defaults.classGap));
    else if (name == "avoided_period")
        unit = std::make_unique<cn::AvoidedPeriod>(w(defaults.avoidedPeriod));
    else if (name == "non_preferred_period")
        unit = std::make_unique<cn::NonPreferredPeriod>(
            w(defaults.nonPreferredPeriod));
    else
    {
        throw py::value_error(
            "unknown constraint '" + name
            + "'; available: unassigned_lesson, class_conflict, "
              "teacher_conflict, slot_not_allowed, room_capacity, "
              "course_daily_limit, block_adjacency, teacher_load_limit, "
              "teacher_consecutive_limit, teacher_gap, teacher_workday, "
              "teacher_free_day, class_gap, avoided_period, "
              "non_preferred_period");
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

void bindCanon(py::module_ &canon)
{
    py::class_<cn::ObjectiveWeights>(canon, "ObjectiveWeights", py::module_local())
        .def(py::init(
                 [](int teacherGap,
                    int teacherWorkday,
                    int teacherFreeDay,
                    int classGap,
                    int avoidedPeriod,
                    int nonPreferredPeriod)
                 {
                     cn::ObjectiveWeights w;
                     w.teacherGap = teacherGap;
                     w.teacherWorkday = teacherWorkday;
                     w.teacherFreeDay = teacherFreeDay;
                     w.classGap = classGap;
                     w.avoidedPeriod = avoidedPeriod;
                     w.nonPreferredPeriod = nonPreferredPeriod;
                     return w;
                 }),
             py::arg("teacher_gap") = 20,
             py::arg("teacher_workday") = 3,
             py::arg("teacher_free_day") = 30,
             py::arg("class_gap") = 10,
             py::arg("avoided_period") = 5,
             py::arg("non_preferred_period") = 1)
        .def_readwrite("teacher_gap", &cn::ObjectiveWeights::teacherGap)
        .def_readwrite("teacher_workday", &cn::ObjectiveWeights::teacherWorkday)
        .def_readwrite("teacher_free_day",
                       &cn::ObjectiveWeights::teacherFreeDay)
        .def_readwrite("class_gap", &cn::ObjectiveWeights::classGap)
        .def_readwrite("avoided_period", &cn::ObjectiveWeights::avoidedPeriod)
        .def_readwrite("non_preferred_period",
                       &cn::ObjectiveWeights::nonPreferredPeriod);

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
        // Per-slot permission table (H5); empty = every slot allowed.
        .def_property(
            "allowed_slot", &allowedSlotToPython, &allowedSlotFromPython)
        // S5/S6 units: one (avoided_mask, preferred_mask) pair per member
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
        // Per-courseKey period masks (bit = period - 1), the default S5/S6
        // preference unit for lessons without explicit preference_units.
        .def_readwrite("course_avoided_period_mask",
                       &cn::Timetable::courseAvoidedPeriodMask)
        .def_readwrite("course_preferred_period_mask",
                       &cn::Timetable::coursePreferredPeriodMask)
        .def_readwrite("weights", &cn::Timetable::weights)
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

    // Per-constraint violation counts (the exact semantics of the CP-SAT
    // package's validate.py: hard H1~H10 counts and raw soft S1~S6 counts).
    py::class_<cn::Breakdown>(canon, "Breakdown", py::module_local())
        .def_readonly("unassigned", &cn::Breakdown::unassigned)
        .def_readonly("class_conflicts", &cn::Breakdown::classConflicts)
        .def_readonly("teacher_conflicts", &cn::Breakdown::teacherConflicts)
        .def_readonly("slot_not_allowed", &cn::Breakdown::slotNotAllowed)
        .def_readonly("room_over_capacity", &cn::Breakdown::roomOverCapacity)
        .def_readonly("course_daily_excess", &cn::Breakdown::courseDailyExcess)
        .def_readonly("block_breaks", &cn::Breakdown::blockBreaks)
        .def_readonly("teacher_daily_excess",
                      &cn::Breakdown::teacherDailyExcess)
        .def_readonly("teacher_weekly_excess",
                      &cn::Breakdown::teacherWeeklyExcess)
        .def_readonly("consecutive_excess", &cn::Breakdown::consecutiveExcess)
        .def_readonly("teacher_gap", &cn::Breakdown::teacherGap)
        .def_readonly("teacher_workday", &cn::Breakdown::teacherWorkday)
        .def_readonly("teacher_free_day", &cn::Breakdown::teacherFreeDay)
        .def_readonly("class_gap", &cn::Breakdown::classGap)
        .def_readonly("avoided_period", &cn::Breakdown::avoidedPeriod)
        .def_readonly("non_preferred_period",
                      &cn::Breakdown::nonPreferredPeriod)
        .def("hard_total", &cn::Breakdown::hardTotal)
        .def("soft_penalty", &cn::Breakdown::softPenalty, py::arg("weights"))
        // The raw soft counts keyed like the CP-SAT Timetable.penalties dict.
        .def("penalties",
             [](const cn::Breakdown &b)
             {
                 py::dict d;
                 d["teacher_gap"] = b.teacherGap;
                 d["teacher_workday"] = b.teacherWorkday;
                 d["teacher_free_day"] = b.teacherFreeDay;
                 d["class_gap"] = b.classGap;
                 d["avoided_period"] = b.avoidedPeriod;
                 d["non_preferred_period"] = b.nonPreferredPeriod;
                 return d;
             });

    canon.def("calculate_score",
              &cn::ScoreCalculator::calculateScore,
              py::arg("table"),
              "Full recompute of the HardSoftScore (also stores table.score).");
    canon.def("compute_breakdown",
              &cn::ScoreCalculator::computeBreakdown,
              py::arg("table"),
              "Full per-constraint recompute (H1~H10 / S1~S6 counts).");
    canon.def("prepare",
              &cn::ScoreCalculator::prepare,
              py::arg("table"),
              "Normalize the instance in place (idempotent; also run by "
              "compute_breakdown / calculate_score / Solver.solve).");

    // ------------------------------------------------------------------
    // Plugin-style cost assembly: pick
    // canonical constraints and weights from Python, then pass the assembly
    // to Solver.solve(table, calc).
    // ------------------------------------------------------------------
    py::class_<cn::DecomposedCalculator>(
        canon,
        "DecomposedCalculator", py::module_local(),
        "Pluggable score calculator: assemble canonical constraints — either "
        "from an iterable of constraint specs (see st_timetabling."
        "constraints) or incrementally via add(name, ...) — then pass to "
        "Solver.solve(table, calc). The full default set (see "
        "default_calculator) reproduces canon.calculate_score exactly when "
        "its weights equal table.weights.")
        .def(py::init<>())
        // Example: DecomposedCalculator([TeacherGap(40), ...]).
        // Duck-typed: each spec needs .name/.weight/.hard/.exponent (the
        // st_timetabling.constraints dataclasses and Assembly items qualify;
        // weight None keeps the component default).
        .def(py::init(
                 [](const py::iterable &constraints)
                 {
                     auto calc = std::make_unique<cn::DecomposedCalculator>();
                     for (py::handle spec : constraints)
                     {
                         addCanonConstraintByName(
                             *calc,
                             spec.attr("name").cast<std::string>(),
                             spec.attr("weight").cast<std::optional<double>>(),
                             spec.attr("hard").cast<std::optional<bool>>(),
                             spec.attr("exponent").cast<double>());
                     }
                     return calc;
                 }),
             py::arg("constraints"),
             "Assemble from constraint spec objects (duck-typed: .name, "
             ".weight, .hard, .exponent), e.g.\n"
             "DecomposedCalculator(Assembly().add('teacher_gap', "
             "weight=40)).")
        .def(
            "add",
            [](cn::DecomposedCalculator &calc,
               const std::string &name,
               std::optional<double> weight,
               std::optional<bool> hard,
               double exponent)
            { addCanonConstraintByName(calc, name, weight, hard, exponent); },
            py::arg("name"),
            py::arg("weight") = std::nullopt,
            py::arg("hard") = std::nullopt,
            py::arg("exponent") = 1.0,
            "Add a built-in canonical constraint by name (see "
            "CONSTRAINT_NAMES).\n"
            "weight:   linear multiplier on the constraint's cost; None keeps "
            "the component default (hard H* = 1, soft S* = the CP-SAT "
            "ObjectiveWeights default).\n"
            "hard:     True/False overrides the constraint's default "
            "hard/soft level (None keeps the default).\n"
            "exponent: sign-preserving power shaping of the per-unit cost: "
            "contribution = weight * sign(c)*|c|^exponent.\n"
            "Raises ValueError for an unknown name. Duplicates are allowed "
            "(their costs add up).")
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
            "Weighted cost per constraint for the last scored/solved "
            "solution (positive = penalizing).")
        .def_property_readonly("delta_group_count",
                               &cn::DecomposedCalculator::deltaGroupCount,
                               "Number of structural delta groups compiled by "
                               "the last reset/solve.");

    canon.attr("CONSTRAINT_NAMES") = py::make_tuple("unassigned_lesson",
                                                    "class_conflict",
                                                    "teacher_conflict",
                                                    "slot_not_allowed",
                                                    "room_capacity",
                                                    "course_daily_limit",
                                                    "block_adjacency",
                                                    "teacher_load_limit",
                                                    "teacher_consecutive_limit",
                                                    "teacher_gap",
                                                    "teacher_workday",
                                                    "teacher_free_day",
                                                    "class_gap",
                                                    "avoided_period",
                                                    "non_preferred_period");

    canon.def(
        "default_calculator",
        [](const cn::ObjectiveWeights &weights)
        {
            auto calc = std::make_unique<cn::DecomposedCalculator>();
            cn::addCanonicalConstraints(*calc, weights);
            return calc;
        },
        py::arg("weights") = cn::ObjectiveWeights{},
        "A DecomposedCalculator preloaded with the full canonical set: "
        "H1~H10 hard at weight 1 plus S1~S6 soft at `weights` (default: the "
        "CP-SAT ObjectiveWeights defaults). With weights == table.weights it "
        "scores bit-for-bit like canon.calculate_score.");

    py::class_<cn::Solver::Config> config(canon, "SolverConfig", py::module_local());
    bindSolverConfigCommon(config);
    // S1~S6 weights, copied onto the Timetable at the start of solve().
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
    // demotes H1 to soft (partial timetabling; see canonical_solver.hpp for the sweep).
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
    // package's hard H1~H10 / soft S1~S6 with validate.py semantics.
    py::module_ canonModule = m.def_submodule(
        "canon", "Canonical full-parity school-timetabling domain (st::canon)");
    bindCanon(canonModule);
}
