// Plugin-assembly (es/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the catalog: defaultConstraints().info() and
//       catalog_info(defaultConstraints(), catalogConstraints()) == each
//       component's kDefaultWeight / kDefaultLevel, reward and flippability;
//       registry names are camelCase and map to the Python names
//   (b) the default assembly reproduces the monolithic score bit-for-bit on
//       random assignments (including unassigned and pinned shifts); a pinned
//       shift by hand (the four per-shift terms skip it, the pair terms and the
//       imbalance count it); a ScoreChecks::Full solve on each path with
//       pinned shifts
//   (c) incremental score == full recompute across random moves driven
//       through the engine's Editor, for an assembly of all 30
//       components at parameters that bind (scoped and repeated instances
//       included)
//   (d) ScopedUnit takes over the inner component's metadata: a scoped
//       reward keeps reward / level_flippable and refuses set_level(Hard)
//   (e) labels: parametrized instances carry their display label in
//       constraint_totals() / constraint_costs(); the registry name stays plain
//   (f) parallel workers: a throwing on_new_best (worker 0) stops the other
//       workers, so a 30 s, 4-worker solve rethrows at once (Python reaches
//       no thread pool here: solve_request forks processes for its starts)
//   (h) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (undesired minutes + 0.5 x undesired minutes^2), threshold and
//       literal offsets by hand (literal mode charges every employee, a
//       worker scope's employees only), the literal formula refused on hard
//       at reset (a scoped one included) and kept on soft, incremental == a
//       fresh recompute over a random walk and a ScoreChecks::Full solve with
//       repeated, shifted components (delta-grouped hard, shared-context day
//       patterns, scoped and global ones)
// The hand-calculated positive and zero raw cost of every constraint is
// checked through the bindings (tests/employee_scheduling/test_constraint_costs.py).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "es/constraints.hpp"
#include "es/domain.hpp"
#include "es/score_calculator.hpp"
#include "es/solver.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/worker_threads.hpp"
#include "optimaru/python/names.hpp"
#include "optimaru/testing/check.hpp"

using namespace es;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

bool sameScore(const Score& a, const Score& b) {
    const double tolerance = 1e-6 * std::max(1.0, std::fabs(b.soft));
    return a.hard == b.hard && std::fabs(a.soft - b.soft) <= tolerance;
}

std::string str(const Score& s) { return s.str(); }

template <typename F>
bool throwsInvalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

Minutes at(int day, int hour) { return static_cast<Minutes>(day) * MINUTES_PER_DAY + hour * 60; }

// 14 days, five shifts a day over two locations and three skills; six
// employees with unavailable / undesired / desired days and a few
// location-specific wishes. Every shift starts unassigned.
EmployeeSchedule buildRoster(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    EmployeeSchedule s;
    const std::vector<std::string> skills = {"Skill A", "Skill B", "Skill C"};
    for (int e = 0; e < 6; ++e) {
        Employee employee;
        employee.name = "Worker " + std::to_string(e + 1);
        for (int k = 0; k < 3; ++k)
            if (k != e % 3 || e == 5) employee.skills.insert(skills[k]);
        employee.unavailableDates = {static_cast<DayIndex>(rng() % 14)};
        employee.undesiredDates = {static_cast<DayIndex>(rng() % 14),
                                   static_cast<DayIndex>(rng() % 14)};
        employee.desiredDates = {static_cast<DayIndex>(rng() % 14)};
        employee.desiredShifts = {{static_cast<DayIndex>(rng() % 14), "Location B"}};
        s.employees.push_back(employee);
    }
    struct Slot {
        int hour;
        const char* location;
        const char* skill;
    };
    const Slot slots[] = {{8, "Location A", "Skill A"},
                          {16, "Location A", "Skill A"},
                          {8, "Location B", "Skill B"},
                          {16, "Location B", "Skill B"},
                          {0, "Location A", "Skill C"}};
    int id = 0;
    for (int d = 0; d < 14; ++d) {
        for (const Slot& slot : slots) {
            Shift shift;
            shift.id = "Shift " + std::to_string(id++);
            shift.start = at(d, slot.hour);
            shift.end = shift.start + 8 * 60;
            shift.location = slot.location;
            shift.requiredSkill = slot.skill;
            s.shifts.push_back(shift);
        }
    }
    return s;
}

void randomAssignment(EmployeeSchedule& s, std::mt19937_64& rng, bool allowUnassigned) {
    const int employees = static_cast<int>(s.employees.size());
    for (Shift& shift : s.shifts) {
        const int value = static_cast<int>(rng() % (employees + (allowUnassigned ? 1 : 0))) -
                          (allowUnassigned ? 1 : 0);
        shift.employeeIndex = value;
    }
}

Score score(DecomposedCalculator& calc, EmployeeSchedule& s) {
    ScoreCalculator::prepare(s);
    calc.rebuild(s);
    return calc.score();
}

// ---------------------------------------------------------------------------
// (a) the catalog
// ---------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           bool is_global, bool enabled, bool reward = false) {
    Component component;  // constructs at its default parameters
    return row.name == name && component.name == name &&
           row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight &&
           component.weight == Component::kDefaultWeight &&
           component.level == Component::kDefaultLevel && row.default_level == level &&
           row.default_weight == 1.0 && row.reward == reward && row.level_flippable == !reward &&
           row.is_global == is_global && row.default_enabled == enabled;
}

void verifyCatalog() {
    std::printf("== (a) catalog ==\n");
    using optimaru::Level;
    const auto defaults = defaultConstraints().info();
    check(defaults.size() == 8 &&
                  rowIs<RequiredSkillCost>(defaults[0], "requiredSkillCost", Level::Hard, false,
                                           true) &&
                  rowIs<EmployeeUnavailabilityCost>(defaults[1], "employeeUnavailabilityCost",
                                                    Level::Hard, false, true) &&
                  rowIs<ShiftOverlapCost>(defaults[2], "shiftOverlapCost", Level::Hard, false,
                                          true) &&
                  rowIs<MaxOneShiftPerDayCost>(defaults[3], "maxOneShiftPerDayCost", Level::Hard,
                                               false, true) &&
                  rowIs<MinRestCost>(defaults[4], "minRestCost", Level::Hard, false, true) &&
                  rowIs<UndesiredDayCost>(defaults[5], "undesiredDayCost", Level::Soft, false,
                                          true) &&
                  rowIs<DesiredDayCost>(defaults[6], "desiredDayCost", Level::Soft, false, true,
                                        true) &&
                  rowIs<ShiftCountImbalanceCost>(defaults[7], "shiftCountImbalanceCost",
                                                 Level::Soft, true, true),
          "defaultConstraints().info() == the eight default components' constants");

    const auto rows = optimaru::catalog_info(defaultConstraints(), catalogConstraints());
    bool defaultsFirst = rows.size() == 30;
    for (std::size_t i = 0; defaultsFirst && i < defaults.size(); ++i)
        defaultsFirst = rows[i].name == defaults[i].name && rows[i].default_enabled;
    check(defaultsFirst, "catalogInfo: 30 rows, the eight defaults first and enabled");
    // ConstraintAssembly::info() lists the unit constraints before the
    // global ones, so the 22 catalog-only rows come as 16 units, then 6
    // globals, each group in factory order.
    check(rows.size() == 30 &&
                  rowIs<DesiredShiftCost>(rows[8], "desiredShiftCost", Level::Soft, false, false,
                                          true) &&
                  rowIs<MaxConsecutiveDaysCost>(rows[9], "maxConsecutiveDaysCost", Level::Hard,
                                                false, false) &&
                  rowIs<MinConsecutiveDaysCost>(rows[10], "minConsecutiveDaysCost", Level::Soft,
                                                false, false) &&
                  rowIs<MaxConsecutiveDaysOffCost>(rows[11], "maxConsecutiveDaysOffCost",
                                                   Level::Soft, false, false) &&
                  rowIs<RestAfterConsecutiveWorkCost>(rows[12], "restAfterConsecutiveWorkCost",
                                                      Level::Hard, false, false) &&
                  rowIs<MaxWeeklyMinutesCost>(rows[13], "maxWeeklyMinutesCost", Level::Hard, false,
                                              false) &&
                  rowIs<MinWeeklyMinutesCost>(rows[14], "minWeeklyMinutesCost", Level::Soft, false,
                                              false) &&
                  rowIs<MinWeeklyRestDaysCost>(rows[15], "minWeeklyRestDaysCost", Level::Hard,
                                               false, false) &&
                  rowIs<AssignmentCost>(rows[16], "assignmentCost", Level::Soft, false, false) &&
                  rowIs<LocationRunLengthCost>(rows[17], "locationRunLengthCost", Level::Soft,
                                               false, false) &&
                  rowIs<RestAfterLocationRunCost>(rows[18], "restAfterLocationRunCost", Level::Hard,
                                                  false, false) &&
                  rowIs<LocationRunGapCost>(rows[19], "locationRunGapCost", Level::Soft, false,
                                            false) &&
                  rowIs<WorkerLocationCountCost>(rows[20], "workerLocationCountCost", Level::Soft,
                                                 false, false) &&
                  rowIs<DayPatternCost>(rows[21], "dayPatternCost", Level::Soft, false, false) &&
                  rowIs<TargetDaysOffCost>(rows[22], "targetDaysOffCost", Level::Soft, false,
                                           false) &&
                  rowIs<WorkerDayCountInRangeCost>(rows[23], "workerDayCountInRangeCost",
                                                   Level::Soft, false, false) &&
                  rowIs<UnassignedShiftsCost>(rows[24], "unassignedShiftsCost", Level::Hard, true,
                                              false) &&
                  rowIs<ShiftCountGapCost>(rows[25], "shiftCountGapCost", Level::Soft, true,
                                           false) &&
                  rowIs<WeeklyMinutesGapCost>(rows[26], "weeklyMinutesGapCost", Level::Soft, true,
                                              false) &&
                  rowIs<ShiftCoverageCost>(rows[27], "shiftCoverageCost", Level::Soft, true,
                                           false) &&
                  rowIs<LocationImbalanceCost>(rows[28], "locationImbalanceCost", Level::Soft, true,
                                               false) &&
                  rowIs<WorkerPairingCost>(rows[29], "workerPairingCost", Level::Soft, true, false),
          "catalogInfo: the 22 rule-only components' constants, disabled by default");

    bool camel = true;
    for (const auto& row : rows) {
        const std::string snake = optimaru::python::python_name(row.name);
        camel = camel && !row.name.empty() &&
                std::islower(static_cast<unsigned char>(row.name[0])) &&
                row.name.find(' ') == std::string::npos && snake.size() > 5 &&
                snake.compare(snake.size() - 5, 5, "_cost") == 0;
    }
    check(camel, "every registry name is camelCase and its Python name ends in _cost");
    check(optimaru::python::python_name("maxConsecutiveDaysOffCost") ==
                          "max_consecutive_days_off_cost" &&
                  optimaru::python::python_name("workerDayCountInRangeCost") ==
                          "worker_day_count_in_range_cost",
          "pythonName maps the registry names to their snake_case Python names");

    check(throwsInvalid([] { defaultConstraints().hard("desiredDayCost"); }),
          "hard() on the reward desiredDayCost throws");
    check(throwsInvalid([] { catalogConstraints().hard("desiredShiftCost"); }),
          "hard() on the reward desiredShiftCost throws");
    DesiredDayCost desired;
    desired.set_level(optimaru::Level::Soft);
    check(desired.level == optimaru::Level::Soft, "re-stating a reward's soft level is a no-op");
    check(!throwsInvalid(
                  [] { defaultConstraints().soft("requiredSkillCost").hard("undesiredDayCost"); }),
          "the other defaults stay flippable both ways");
    check(throwsInvalid([] { WorkerDayCountInRangeCost bad(1.0, -1, 5); }),
          "WorkerDayCountInRangeCost refuses a day range outside the horizon limit");
}

// ---------------------------------------------------------------------------
// (b) default assembly == monolith
// ---------------------------------------------------------------------------
void verifyDefaultAssemblyEqualsMonolith() {
    std::printf("== (b) default assembly == monolithic score ==\n");
    EmployeeSchedule s = buildRoster(3);
    std::mt19937_64 rng(17);
    int mismatches = 0;
    for (int round = 0; round < 300; ++round) {
        randomAssignment(s, rng, /*allowUnassigned=*/round % 3 == 0);
        // Every fifth assigned shift pinned (from round 100 on), so a pinned
        // shift meets every per-shift and pair term at random.
        for (std::size_t i = 0; i < s.shifts.size(); ++i)
            s.shifts[i].pinned = round >= 100 && i % 5 == static_cast<std::size_t>(round % 5) &&
                                 s.shifts[i].employeeIndex >= 0;
        EmployeeSchedule copy = s;
        const Score monolithic = ScoreCalculator::score(copy);
        DecomposedCalculator calc;
        addDefaultConstraints(calc);
        const Score plugin = score(calc, s);
        if (!(monolithic.hard == plugin.hard && monolithic.soft == plugin.soft)) {
            if (++mismatches <= 3)
                std::printf("    round %d: monolith %s plugin %s\n", round, str(monolithic).c_str(),
                            str(plugin).c_str());
        }
    }
    check(mismatches == 0,
          "300 random assignments (200 with pinned shifts): the default assembly equals the "
          "monolith bit-for-bit");
}

// A pinned shift is settled: the four per-shift terms (required skill,
// unavailability, undesired and desired day) skip it on both paths, the pair
// terms and the shift-count imbalance count it. Worker 1 lacks Skill A, is
// unavailable on day 1, avoids day 2 and wants day 3; Worker 2 has Skill A.
EmployeeSchedule pinnedInstance() {
    EmployeeSchedule s;
    s.employees.push_back(Employee{"Worker 1", {}, {1}, {2}, {3}, {}});
    s.employees.push_back(Employee{"Worker 2", {"Skill A"}, {}, {}, {}, {}});
    auto add = [&](int day, int hour, int hours, const char* skill, bool pinned) {
        Shift shift;
        shift.id = "Shift " + std::to_string(s.shifts.size());
        shift.start = at(day, hour);
        shift.end = shift.start + hours * 60;
        shift.requiredSkill = skill;
        shift.employeeIndex = 0;
        shift.pinned = pinned;
        s.shifts.push_back(shift);
    };
    add(0, 9, 8, "Skill A", true);  // missing skill
    add(1, 9, 8, "", true);         // unavailable day: 480 minutes
    add(2, 9, 8, "", true);         // undesired day: 480 minutes
    add(3, 9, 8, "", true);         // desired day: 480 minutes
    add(5, 9, 8, "", true);         // overlaps the next one by 240 minutes, same day
    add(5, 13, 8, "", true);
    add(8, 9, 8, "Skill A", false);  // movable: its missing skill counts
    return s;
}

void verifyPinnedShifts() {
    std::printf("== (b2) pinned shifts on both paths ==\n");
    // Hard: the movable missing skill (1) + the pinned overlap (240) + one shift
    // per day (1). Soft: imbalance over (7, 0), sqrt(49 - 49 / 2) = 4.94975.
    const Score expected{-242, -4.94975};
    {
        EmployeeSchedule mono = pinnedInstance();
        const Score monolithic = ScoreCalculator::score(mono);
        EmployeeSchedule plug = pinnedInstance();
        DecomposedCalculator calc;
        addDefaultConstraints(calc);
        const Score plugin = score(calc, plug);
        std::printf("    monolith %s plugin %s\n", str(monolithic).c_str(), str(plugin).c_str());
        check(sameScore(monolithic, expected),
              "monolith: pinned per-shift terms skipped, -242hard");
        check(sameScore(plugin, expected), "default assembly: the same score");
    }
    // A solve on each path under ScoreChecks::Full: no move picks a pinned shift,
    // so the incremental score never adds or drops its per-shift terms.
    for (const bool plugin : {false, true}) {
        EmployeeSchedule s = buildRoster(23);
        std::mt19937_64 rng(31);
        randomAssignment(s, rng, /*allowUnassigned=*/false);
        for (std::size_t i = 0; i < s.shifts.size(); i += 3) s.shifts[i].pinned = true;
        Solver::Config config;
        config.time_limit = std::chrono::seconds(10);
        config.step_limit = 20000;
        config.seed = 7;
        config.score_checks = optimaru::ScoreChecks::Full;
        bool corrupted = false;
        try {
            Solver solver(config);
            if (plugin) {
                DecomposedCalculator calc;
                addDefaultConstraints(calc);
                solver.solve(s, calc);
            } else {
                solver.solve(s);
            }
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        EmployeeSchedule copy = s;
        const Score monolithic = ScoreCalculator::score(copy);
        const std::string path = plugin ? "default assembly" : "monolithic";
        check(!corrupted && sameScore(s.score, monolithic),
              "20000-step FULL_ASSERT " + path +
                      " solve with every third shift pinned: "
                      "the reported score == a monolithic recompute");
    }
}

// ---------------------------------------------------------------------------
// (c) incremental == full for all 30 components
// ---------------------------------------------------------------------------
void addEveryComponent(DecomposedCalculator& calc) {
    calc.emplace<RequiredSkillCost>();
    calc.emplace<EmployeeUnavailabilityCost>();
    calc.emplace<ShiftOverlapCost>();
    calc.emplace<MaxOneShiftPerDayCost>();
    calc.emplace<MinRestCost>(1.0, 12 * 60);
    calc.emplace<UndesiredDayCost>(2.0, /*perDay=*/true);
    calc.emplace<DesiredDayCost>(1.0, /*perDay=*/true);
    calc.emplace<DesiredShiftCost>();
    calc.emplace<ShiftCountImbalanceCost>();
    calc.emplace<MaxConsecutiveDaysCost>(1.0, 4);
    calc.emplace<MinConsecutiveDaysCost>(1.0, 2);
    calc.emplace<MaxConsecutiveDaysOffCost>(1.0, 2);
    calc.emplace<RestAfterConsecutiveWorkCost>(1.0, 3, 2);
    calc.emplace<MaxWeeklyMinutesCost>(1.0, 1800, 2);
    calc.emplace<MinWeeklyMinutesCost>(0.5, 1200, 2);
    calc.emplace<MinWeeklyRestDaysCost>(1.0, 2);
    calc.emplace<AssignmentCost>(0.01, 10.0, 5.0);
    calc.emplace<UnassignedShiftsCost>(50.0);
    calc.emplace<ShiftCountGapCost>(1.0, 1);
    calc.emplace<WeeklyMinutesGapCost>(1.0, 60);
    calc.emplace<ShiftCoverageCost>(1.0, "Skill A", 1, 3);
    calc.emplace<LocationRunLengthCost>(1.0, "Location A", 2, 3);
    calc.emplace<RestAfterLocationRunCost>(1.0, "Location A", 2);
    calc.emplace<LocationRunGapCost>(1.0, "Location B", 3);
    calc.emplace<WorkerLocationCountCost>(1.0, "Location B", "", 2, 6);
    calc.emplace<DayPatternCost>(1.0, "Location A,OFF,Location B");
    calc.emplace<DayPatternCost>(1.0, "WORK,WORK,WORK");  // a second instance of one name
    calc.emplace<TargetDaysOffCost>(1.0, std::vector<int>{5, 6, 12, 13}, 2);
    calc.emplace<WorkerDayCountInRangeCost>(1.0, 0, 6, 2, 4);
    calc.emplace<LocationImbalanceCost>(1.0, "Location A", "", "ssd", 0, std::vector<int>{});
    calc.emplace<LocationImbalanceCost>(1.0, "", "Skill B", "gap", 1, std::vector<int>{5, 6});
    calc.emplace<WorkerPairingCost>(
            1.0, 0, 1, std::vector<int>{0, 1, 2, 3},
            std::vector<std::string>{"Location A", "Location A", "Location B", "Location B"},
            false);
    calc.add(std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 2),
                                          std::vector<int>{0, 2}, std::vector<int>{}, true, false));
    calc.add(std::make_unique<ScopedUnit>(std::make_unique<UndesiredDayCost>(), std::vector<int>{},
                                          std::vector<int>{0, 2, 4, 6, 8}, false, true));
}

void verifyIncrementalEqualsFull() {
    std::printf("== (c) incremental == full recompute, every component ==\n");
    EmployeeSchedule s = buildRoster(5);
    std::mt19937_64 rng(29);
    randomAssignment(s, rng, /*allowUnassigned=*/true);
    ScoreCalculator::prepare(s);
    DecomposedCalculator calc;
    addEveryComponent(calc);
    optimaru::Editor<EmployeeSchedule> editor(s, calc);
    const int employees = static_cast<int>(s.employees.size());
    const int shifts = static_cast<int>(s.shifts.size());
    int mismatches = 0;
    bool anyHard = false, anySoft = false;
    const int steps = 2000;
    for (int step = 0; step < steps; ++step) {
        const int shift = static_cast<int>(rng() % shifts);
        if (rng() % 4 == 0) {
            const int other = static_cast<int>(rng() % shifts);
            const int a = s.shifts[shift].employeeIndex, b = s.shifts[other].employeeIndex;
            editor.assign(shift, b);
            editor.assign(other, a);
        } else {
            editor.assign(shift, static_cast<int>(rng() % (employees + 1)) - 1);
        }
        const Score incremental = editor.score();
        EmployeeSchedule shadow = s;
        DecomposedCalculator fresh;
        addEveryComponent(fresh);
        const Score full = score(fresh, shadow);
        anyHard = anyHard || full.hard != 0;
        anySoft = anySoft || full.soft != 0.0;
        if (!sameScore(incremental, full) && ++mismatches <= 3)
            std::printf("    step %d: incremental %s full %s\n", step, str(incremental).c_str(),
                        str(full).c_str());
    }
    check(mismatches == 0 && anyHard && anySoft,
          std::to_string(steps) + " random moves: incremental matched the full recompute");
}

// ---------------------------------------------------------------------------
// (d) ScopedUnit metadata
// ---------------------------------------------------------------------------
void verifyScopedUnit() {
    std::printf("== (d) ScopedUnit keeps the inner metadata ==\n");
    ScopedUnit reward(std::make_unique<DesiredDayCost>(2.5), std::vector<int>{0},
                      std::vector<int>{}, true, false);
    check(reward.name == "desiredDayCost" && reward.reward && !reward.level_flippable &&
                  reward.weight == 2.5 && reward.level == optimaru::Level::Soft,
          "a scoped DesiredDayCost keeps name, reward, levelFlippable, weight and level");
    check(throwsInvalid([&] { reward.set_level(optimaru::Level::Hard); }),
          "a scoped reward refuses setLevel(Hard)");
    ScopedUnit pattern(std::make_unique<DayPatternCost>(1.0, "OFF,Location A"), std::vector<int>{},
                       std::vector<int>{1, 2}, false, true);
    check(pattern.name == "dayPatternCost" && pattern.label == "Day pattern[OFF,Location A]" &&
                  pattern.level_flippable && !pattern.reward,
          "a scoped DayPatternCost keeps its label and stays flippable");
    pattern.set_level(optimaru::Level::Hard);
    check(pattern.level == optimaru::Level::Hard, "the wrapper's own level is what setLevel moves");
}

// ---------------------------------------------------------------------------
// (e) labels
// ---------------------------------------------------------------------------
void verifyLabels() {
    std::printf("== (e) labels ==\n");
    check(ShiftCoverageCost(1.0, "Skill A", 1, -1).label == "Shift coverage[Skill A 1..inf]" &&
                  ShiftCoverageCost().label == "Shift coverage[any 0..inf]",
          "ShiftCoverageCost label");
    check(LocationRunLengthCost(1.0, "Location B", 3, 6).label == "Location run[Location B 3..6]" &&
                  LocationRunLengthCost().label == "Location run[work 0..inf]",
          "LocationRunLengthCost label");
    check(RestAfterLocationRunCost(1.0, "Location B", 3).label ==
                  "Rest after location run[Location B 3]",
          "RestAfterLocationRunCost label");
    check(LocationRunGapCost(1.0, "", 10).label == "Location run gap[work 10]",
          "LocationRunGapCost label");
    check(WorkerLocationCountCost(1.0, "Location B", "Skill B", 4, 8).label ==
                  "Worker location count[Location B Skill B 4..8]",
          "WorkerLocationCountCost label");
    check(DayPatternCost(1.0, "Location A,OFF,Location B").label ==
                  "Day pattern[Location A,OFF,Location B]",
          "DayPatternCost label");
    check(TargetDaysOffCost(1.0, {5, 6}, 1).label == "Target days off[>=1]",
          "TargetDaysOffCost label");
    check(WorkerDayCountInRangeCost(1.0, 0, 6, 2, -1).label == "Worker day count[work 0..6 2..inf]",
          "WorkerDayCountInRangeCost label");
    check(LocationImbalanceCost(1.0, "", "Skill A", "gap", 1, {5}).label ==
                          "Balance[off-target Skill A gap]" &&
                  LocationImbalanceCost().label == "Balance[work all ssd]",
          "LocationImbalanceCost label");
    check(WorkerPairingCost(1.0, 0, 1, {}, {}, true).label == "Worker pairing[0!=1]",
          "WorkerPairingCost label");
    check(RequiredSkillCost().label.empty() && MinRestCost(1.0, 720).label.empty(),
          "unparametrized-label components leave the label empty");

    EmployeeSchedule s = buildRoster(7);
    std::mt19937_64 rng(3);
    randomAssignment(s, rng, false);
    DecomposedCalculator calc;
    calc.emplace<DayPatternCost>(1.0, "WORK,WORK");
    calc.emplace<DayPatternCost>(1.0, "OFF,OFF");
    score(calc, s);
    const auto totals = calc.constraint_totals();
    const auto costs = calc.constraint_costs();
    check(totals.size() == 2 && totals[0].name == "dayPatternCost" &&
                  totals[1].name == "dayPatternCost" &&
                  totals[0].label == "Day pattern[WORK,WORK]" &&
                  totals[1].label == "Day pattern[OFF,OFF]" && costs.size() == 2 &&
                  costs[1].label == "Day pattern[OFF,OFF]",
          "two instances of one registry name report the plain name and their own labels");
}

// ---------------------------------------------------------------------------
// (f) a failing parallel worker stops the others
// ---------------------------------------------------------------------------
void verifyParallelFailureStopsWorkers() {
    std::printf("== (f) a failing parallel worker stops the others ==\n");
    EmployeeSchedule s = buildRoster(5);
    Solver::Config config;
    config.time_limit = std::chrono::seconds(30);
    config.parallel_solver_count = 4;
    config.on_new_best = [](std::int64_t, const Score&) { throw std::runtime_error("boom"); };
    const auto started = std::chrono::steady_clock::now();
    bool raised = false;
    try {
        Solver(config).solve(s);
    } catch (const std::runtime_error& error) {
        raised = std::string(error.what()) == "boom";
    }
    const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    check(raised && seconds < 10.0,
          "4 workers, 30 s budget: worker 0's exception reaches the caller after " +
                  std::to_string(seconds) + " s");
}

void verifyThreadStartFailureStopsWorkers() {
    std::printf("== (g) a worker thread that cannot start ==\n");
    EmployeeSchedule s = buildRoster(5);
    Solver::Config config;
    config.time_limit = std::chrono::seconds(30);
    config.parallel_solver_count = 4;
    // std::thread throws std::system_error when the system has no thread to
    // give; here worker 1's creation fails through the engine's test seam.
    optimaru::worker_thread_start_hook = [](int index) {
        if (index == 1) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "injected worker thread creation failure");
        }
    };
    const auto started = std::chrono::steady_clock::now();
    bool raised = false;
    try {
        Solver(config).solve(s);
    } catch (const std::system_error& error) {
        raised = std::string(error.what()).find("injected") != std::string::npos;
    }
    optimaru::worker_thread_start_hook = nullptr;
    const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    check(raised && seconds < 10.0,
          "worker 1 cannot start: worker 0 is stopped and joined, the error reaches the caller "
          "after " +
                  std::to_string(seconds) + " s (no std::terminate)");
}

// ---------------------------------------------------------------------------
// (h) repeated components and offsets
// ---------------------------------------------------------------------------
// The bindings build every pick as its own component (the factory), so a
// repeat is simply a second component of the same type, and the knobs are
// re-stated on it (apply_knobs: set_weight / set_exponent / set_offset / set_clip).
template <typename C, typename... Args>
std::unique_ptr<C> shaped(double offset, bool clip, Args&&... args) {
    auto component = std::make_unique<C>(std::forward<Args>(args)...);
    component->set_offset(offset);
    component->set_clip(clip);
    return component;
}

// Two employees, one shift each, all of the 480 minutes of Worker 1's shift on
// one of its undesired days: undesired [480, 0], unavailable [0, 0], loads
// [1, 1] (shift_count_imbalance_cost 0).
EmployeeSchedule repeatInstance() {
    EmployeeSchedule s;
    Employee first;
    first.name = "Worker 1";
    first.skills = {"Skill A"};
    first.undesiredDates = {1};
    Employee second;
    second.name = "Worker 2";
    second.skills = {"Skill A"};
    s.employees = {first, second};
    for (int d = 1; d <= 2; ++d) {
        Shift shift;
        shift.id = "Shift " + std::to_string(d);
        shift.start = at(d, 9);
        shift.end = at(d, 17);
        shift.location = "Location A";
        shift.requiredSkill = "Skill A";
        shift.employeeIndex = d - 1;
        s.shifts.push_back(shift);
    }
    ScoreCalculator::prepare(s);
    return s;
}

void addRepeatedShifted(DecomposedCalculator& calc) {
    addEveryComponent(calc);
    // Hard refuses the literal formula with an offset (hard 0 must mean no
    // violation): the hard copies are thresholds, the literal ones soft.
    calc.add(shaped<RequiredSkillCost>(1.0, true, 2.0));  // delta-grouped hard, threshold
    auto literalSkill = shaped<RequiredSkillCost>(1.0, false, 2.0);  // ... and a soft literal copy
    literalSkill->set_level(optimaru::Level::Soft);
    calc.add(std::move(literalSkill));
    calc.add(shaped<EmployeeUnavailabilityCost>(60.0, true));    // hard threshold
    auto squared = shaped<UndesiredDayCost>(120.0, true, 0.01);  // a repeat: (minutes - 120)^2
    squared->set_exponent(2.0);
    calc.add(std::move(squared));
    calc.add(shaped<UndesiredDayCost>(-30.0, false, 0.5));  // ... and a literal one
    calc.add(shaped<DesiredDayCost>(60.0, false));          // a reward, literal
    calc.add(shaped<DayPatternCost>(-1.0, true, 1.0,
                                    "WORK,WORK,WORK"));          // shared day-pattern context
    calc.add(shaped<ShiftCountImbalanceCost>(0.5, false, 2.0));  // global
    calc.add(shaped<LocationImbalanceCost>(1.0, false, 1.0, "Location A", "", "ssd", 0,
                                           std::vector<int>{}));
    auto scoped =
            std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 3),
                                         std::vector<int>{1, 3}, std::vector<int>{}, true, false);
    scoped->set_offset(2.0);
    scoped->set_clip(false);
    scoped->set_level(optimaru::Level::Soft);  // a scoped literal copy, on soft
    calc.add(std::move(scoped));
    // Repeats on another level than their default: a hard one soft, a soft one hard.
    auto demoted = shaped<EmployeeUnavailabilityCost>(30.0, false, 0.5);
    demoted->set_level(optimaru::Level::Soft);
    calc.add(std::move(demoted));
    auto promoted = shaped<UndesiredDayCost>(240.0, true);
    promoted->set_level(optimaru::Level::Hard);
    calc.add(std::move(promoted));
}

void verifyRepeatsAndOffsets() {
    std::printf("== (h) repeated components, offset / clip ==\n");
    {
        EmployeeSchedule s = repeatInstance();
        DecomposedCalculator calc;
        calc.emplace<UndesiredDayCost>();
        auto second = std::make_unique<UndesiredDayCost>(0.5);
        second->set_exponent(2.0);
        calc.add(std::move(second));
        const Score result = score(calc, s);
        check(result.hard == 0 && result.soft == -(480.0 + 0.5 * 480.0 * 480.0),
              "undesired + 0.5 x undesired^2: -(480 + 115200) soft");
        const auto entries = calc.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{480.0, 0.0} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 2.0 &&
                      entries[0].name == entries[1].name,
              "two instances, the same raw costs, each with its own knobs");
    }
    {
        EmployeeSchedule s = repeatInstance();
        auto soft = [&](std::unique_ptr<EsUnitConstraint> component) {
            DecomposedCalculator calc;
            calc.add(std::move(component));
            return score(calc, s);
        };
        check(soft(shaped<UndesiredDayCost>(100.0, true)).soft == -380.0,
              "threshold offset 100: 480 - 100");
        check(soft(shaped<UndesiredDayCost>(100.0, false)).soft == -280.0,
              "literal offset 100: (480 - 100) + (0 - 100), both employees");
        check(soft(shaped<UndesiredDayCost>(-10.0, true)).soft == -490.0,
              "threshold offset -10: the non-zero employee only");
        check(soft(shaped<UndesiredDayCost>(-10.0, false)).soft == -500.0,
              "literal offset -10: (480 + 10) + (0 + 10)");
        check(soft(shaped<DesiredDayCost>(30.0, false)).soft == 60.0,
              "a literal offset on a reward (raw [0, 0]): two employees rewarded 30 each");
        // A hard pick rounds per employee: 3 x (480 - 479.5) = 1.5 -> 2.
        auto hard = shaped<UndesiredDayCost>(479.5, true, 3.0);
        hard->set_level(optimaru::Level::Hard);
        check(soft(std::move(hard)).hard == -2,
              "hard threshold offset 479.5 at weight 3: llround(1.5) = 2");
        // The literal unavailability pick on soft charges both employees,
        // 3 x (0 - 0.5) = -1.5 each, not rounded: +3 soft.
        auto literal = shaped<EmployeeUnavailabilityCost>(0.5, false, 3.0);
        literal->set_level(optimaru::Level::Soft);
        const Score literalScore = soft(std::move(literal));
        check(literalScore.hard == 0 && literalScore.soft == 3.0,
              "soft literal offset 0.5 at weight 3 on unavailability: 2 x 1.5 = +3 soft");
        // A worker scope: only Worker 2 is evaluated, so the literal charges it alone.
        DecomposedCalculator scopedCalc;
        auto scoped = std::make_unique<ScopedUnit>(std::make_unique<UndesiredDayCost>(),
                                                   std::vector<int>{1, 1, 7}, std::vector<int>{},
                                                   true, false);
        scoped->set_offset(100.0);
        scoped->set_clip(false);
        scopedCalc.add(std::move(scoped));
        const Score scopedScore = score(scopedCalc, s);
        const auto scopedEntries = scopedCalc.constraint_costs();
        check(scopedScore.soft == 100.0 && scopedEntries.size() == 1 && scopedEntries[0].scoped &&
                      scopedEntries[0].units == std::vector<int>{1},
              "a worker-scoped literal offset charges its in-range employee only (units {1})");
        // A global total (the imbalance of loads [1, 1] is 0) is shaped once.
        DecomposedCalculator global;
        global.add(shaped<ShiftCountImbalanceCost>(2.0, false));
        DecomposedCalculator globalThreshold;
        globalThreshold.add(shaped<ShiftCountImbalanceCost>(2.0, true));
        check(score(global, s).soft == 2.0 && score(globalThreshold, s).soft == 0.0,
              "global total 0, offset 2: literal rewards 2, threshold 0");
    }
    {
        // On hard (its default level, or moved there) the literal formula
        // with an offset is refused at reset, whichever sign and setter
        // order: +0.5 would reward both employees without unavailable minutes
        // (+4 hard, cancelling other hard violations), -0.5 would charge them.
        auto resetRefusal = [](std::unique_ptr<EsUnitConstraint> component) {
            EmployeeSchedule s = repeatInstance();
            DecomposedCalculator calc;
            calc.add(std::move(component));
            try {
                score(calc, s);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts toward "
                   "the "
                   "hard score (hard 0 must mean no violation)";
        };
        auto clipFirst = std::make_unique<EmployeeUnavailabilityCost>(3.0);
        clipFirst->set_clip(false);
        clipFirst->set_offset(-0.5);
        check(resetRefusal(shaped<EmployeeUnavailabilityCost>(0.5, false, 3.0)) ==
                              refusal("employeeUnavailabilityCost") &&
                      resetRefusal(std::move(clipFirst)) == refusal("employeeUnavailabilityCost"),
              "unavailability literal offset +-0.5 on hard: refused at reset");
        auto promoted = shaped<UndesiredDayCost>(100.0, false);
        promoted->set_level(optimaru::Level::Hard);
        auto scoped =
                std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 3),
                                             std::vector<int>{1}, std::vector<int>{}, true, false);
        scoped->set_offset(2.0);
        scoped->set_clip(false);
        check(resetRefusal(std::move(promoted)) == refusal("undesiredDayCost") &&
                      resetRefusal(std::move(scoped)) == refusal("maxConsecutiveDaysCost"),
              "a soft literal offset moved to hard, and a scoped hard one: refused at reset");
        {
            // Next to the default component, the literal hard repeat is refused too.
            EmployeeSchedule s = repeatInstance();
            DecomposedCalculator calc;
            calc.emplace<EmployeeUnavailabilityCost>();
            calc.add(shaped<EmployeeUnavailabilityCost>(0.5, false));
            std::string message;
            try {
                score(calc, s);
            } catch (const std::logic_error& error) {
                message = error.what();
            }
            check(message == refusal("employeeUnavailabilityCost"),
                  "a literal hard repeat next to the default: refused at reset");
        }
        auto softLiteral = shaped<EmployeeUnavailabilityCost>(0.5, false);
        softLiteral->set_level(optimaru::Level::Soft);
        check(resetRefusal(shaped<EmployeeUnavailabilityCost>(0.5, true)).empty() &&
                      resetRefusal(shaped<EmployeeUnavailabilityCost>(0.0, false)).empty() &&
                      resetRefusal(std::move(softLiteral)).empty(),
              "unavailability: a threshold offset or clip=false with offset 0 on hard, and the "
              "literal offset on soft, are allowed");
    }
    {
        // Random walk: incremental == a fresh calculator of the same components.
        EmployeeSchedule s = buildRoster(13);
        std::mt19937_64 rng(31);
        randomAssignment(s, rng, /*allowUnassigned=*/true);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calc;
        addRepeatedShifted(calc);
        optimaru::Editor<EmployeeSchedule> editor(s, calc);
        const int employees = static_cast<int>(s.employees.size());
        const int shifts = static_cast<int>(s.shifts.size());
        int mismatches = 0;
        const int steps = 2000;
        for (int step = 0; step < steps; ++step) {
            const int shift = static_cast<int>(rng() % shifts);
            if (rng() % 4 == 0) {
                const int other = static_cast<int>(rng() % shifts);
                const int a = s.shifts[shift].employeeIndex, b = s.shifts[other].employeeIndex;
                editor.assign(shift, b);
                editor.assign(other, a);
            } else {
                editor.assign(shift, static_cast<int>(rng() % (employees + 1)) - 1);
            }
            const Score incremental = editor.score();
            EmployeeSchedule shadow = s;
            DecomposedCalculator fresh;
            addRepeatedShifted(fresh);
            const Score full = score(fresh, shadow);
            if (!optimaru::scores_agree(incremental, full) && ++mismatches <= 3)
                std::printf("    step %d: incremental %s full %s\n", step, str(incremental).c_str(),
                            str(full).c_str());
        }
        check(mismatches == 0,
              std::to_string(steps) +
                      " random moves with repeated, shifted components: incremental == fresh");
    }
    {
        EmployeeSchedule s = buildRoster(17);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
        config.score_checks = optimaru::ScoreChecks::Full;
        DecomposedCalculator calc;
        addRepeatedShifted(calc);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(s, calc);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        addRepeatedShifted(fresh);
        check(!corrupted && sameScore(s.score, score(fresh, s)),
              "the reported score == a fresh recompute of the same components");
    }
}

}  // namespace

int main() {
    verifyCatalog();
    verifyDefaultAssemblyEqualsMonolith();
    verifyPinnedShifts();
    verifyIncrementalEqualsFull();
    verifyScopedUnit();
    verifyLabels();
    verifyParallelFailureStopsWorkers();
    verifyThreadStartFailureStopsWorkers();
    verifyRepeatsAndOffsets();
    if (failures == 0) {
        std::printf("All plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d plugin check(s) FAILED.\n", failures);
    return 1;
}
