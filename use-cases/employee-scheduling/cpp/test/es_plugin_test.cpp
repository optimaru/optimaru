// Plugin-assembly (es/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the catalog: defaultConstraints().info() and
//       catalogInfo(defaultConstraints(), catalogConstraints()) == each
//       component's kDefaultWeight / kDefaultLevel, reward and flippability;
//       registry names are camelCase and map to the Python names
//   (b) the default assembly reproduces the monolithic score bit-for-bit on
//       random assignments (including unassigned shifts)
//   (c) incremental score == full recompute across random moves driven
//       through the engine's ScoreDirector, for an assembly of all 30
//       components at parameters that bind (scoped and repeated instances
//       included)
//   (d) ScopedUnit takes over the inner component's metadata: a scoped
//       reward keeps reward / levelFlippable and refuses setLevel(Hard)
//   (e) labels: parametrized instances carry their display label in
//       constraintTotals() / constraintCosts(); the registry name stays plain
// The hand-calculated positive and zero raw cost of every constraint is
// checked through the bindings (python/tests/test_constraint_costs.py).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "es/constraints.hpp"
#include "es/domain.hpp"
#include "es/score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/python/names.hpp"

using namespace es;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool sameScore(const Score& a, const Score& b) {
    const double tolerance = 1e-6 * std::max(1.0, std::fabs(b.soft));
    return a.hard == b.hard && std::fabs(a.soft - b.soft) <= tolerance;
}

std::string str(const Score& s) { return s.toString(); }

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
        employee.undesiredDates = {static_cast<DayIndex>(rng() % 14), static_cast<DayIndex>(rng() % 14)};
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
        const int value = static_cast<int>(rng() % (employees + (allowUnassigned ? 1 : 0))) - (allowUnassigned ? 1 : 0);
        shift.employeeIndex = value;
    }
}

Score score(DecomposedCalculator& calc, EmployeeSchedule& s) {
    ScoreCalculator::prepare(s);
    calc.resetWorkingSolution(s);
    return calc.calculateScore();
}

// ---------------------------------------------------------------------------
// (a) the catalog
// ---------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level, bool isGlobal,
           bool enabled, bool reward = false) {
    Component component;  // constructs at its default parameters
    return row.name == name && component.name == name && row.defaultLevel == Component::kDefaultLevel &&
           row.defaultWeight == Component::kDefaultWeight && component.weight == Component::kDefaultWeight &&
           component.level == Component::kDefaultLevel && row.defaultLevel == level &&
           row.defaultWeight == 1.0 && row.reward == reward && row.levelFlippable == !reward &&
           row.isGlobal == isGlobal && row.defaultEnabled == enabled;
}

void verifyCatalog() {
    std::printf("== (a) catalog ==\n");
    using optimaru::Level;
    const auto defaults = defaultConstraints().info();
    check(defaults.size() == 8 &&
                  rowIs<RequiredSkillCost>(defaults[0], "requiredSkillCost", Level::Hard, false, true) &&
                  rowIs<EmployeeUnavailabilityCost>(defaults[1], "employeeUnavailabilityCost", Level::Hard,
                                                    false, true) &&
                  rowIs<ShiftOverlapCost>(defaults[2], "shiftOverlapCost", Level::Hard, false, true) &&
                  rowIs<MaxOneShiftPerDayCost>(defaults[3], "maxOneShiftPerDayCost", Level::Hard, false,
                                               true) &&
                  rowIs<MinRestCost>(defaults[4], "minRestCost", Level::Hard, false, true) &&
                  rowIs<UndesiredDayCost>(defaults[5], "undesiredDayCost", Level::Soft, false, true) &&
                  rowIs<DesiredDayCost>(defaults[6], "desiredDayCost", Level::Soft, false, true, true) &&
                  rowIs<ShiftCountImbalanceCost>(defaults[7], "shiftCountImbalanceCost", Level::Soft, true,
                                                 true),
          "defaultConstraints().info() == the eight default components' constants");

    const auto rows = optimaru::catalogInfo(defaultConstraints(), catalogConstraints());
    bool defaultsFirst = rows.size() == 30;
    for (std::size_t i = 0; defaultsFirst && i < defaults.size(); ++i)
        defaultsFirst = rows[i].name == defaults[i].name && rows[i].defaultEnabled;
    check(defaultsFirst, "catalogInfo: 30 rows, the eight defaults first and enabled");
    // ConstraintAssembly::info() lists the unit constraints before the
    // global ones, so the 22 catalog-only rows come as 16 units, then 6
    // globals, each group in factory order.
    check(rows.size() == 30 &&
                  rowIs<DesiredShiftCost>(rows[8], "desiredShiftCost", Level::Soft, false, false, true) &&
                  rowIs<MaxConsecutiveDaysCost>(rows[9], "maxConsecutiveDaysCost", Level::Hard, false, false) &&
                  rowIs<MinConsecutiveDaysCost>(rows[10], "minConsecutiveDaysCost", Level::Soft, false, false) &&
                  rowIs<MaxConsecutiveDaysOffCost>(rows[11], "maxConsecutiveDaysOffCost", Level::Soft, false,
                                                   false) &&
                  rowIs<RestAfterConsecutiveWorkCost>(rows[12], "restAfterConsecutiveWorkCost", Level::Hard,
                                                      false, false) &&
                  rowIs<MaxWeeklyMinutesCost>(rows[13], "maxWeeklyMinutesCost", Level::Hard, false, false) &&
                  rowIs<MinWeeklyMinutesCost>(rows[14], "minWeeklyMinutesCost", Level::Soft, false, false) &&
                  rowIs<MinWeeklyRestDaysCost>(rows[15], "minWeeklyRestDaysCost", Level::Hard, false, false) &&
                  rowIs<AssignmentCost>(rows[16], "assignmentCost", Level::Soft, false, false) &&
                  rowIs<LocationRunLengthCost>(rows[17], "locationRunLengthCost", Level::Soft, false, false) &&
                  rowIs<RestAfterLocationRunCost>(rows[18], "restAfterLocationRunCost", Level::Hard, false,
                                                  false) &&
                  rowIs<LocationRunGapCost>(rows[19], "locationRunGapCost", Level::Soft, false, false) &&
                  rowIs<WorkerLocationCountCost>(rows[20], "workerLocationCountCost", Level::Soft, false,
                                                 false) &&
                  rowIs<DayPatternCost>(rows[21], "dayPatternCost", Level::Soft, false, false) &&
                  rowIs<TargetDaysOffCost>(rows[22], "targetDaysOffCost", Level::Soft, false, false) &&
                  rowIs<WorkerDayCountInRangeCost>(rows[23], "workerDayCountInRangeCost", Level::Soft, false,
                                                   false) &&
                  rowIs<UnassignedShiftsCost>(rows[24], "unassignedShiftsCost", Level::Hard, true, false) &&
                  rowIs<ShiftCountGapCost>(rows[25], "shiftCountGapCost", Level::Soft, true, false) &&
                  rowIs<WeeklyMinutesGapCost>(rows[26], "weeklyMinutesGapCost", Level::Soft, true, false) &&
                  rowIs<ShiftCoverageCost>(rows[27], "shiftCoverageCost", Level::Soft, true, false) &&
                  rowIs<LocationImbalanceCost>(rows[28], "locationImbalanceCost", Level::Soft, true, false) &&
                  rowIs<WorkerPairingCost>(rows[29], "workerPairingCost", Level::Soft, true, false),
          "catalogInfo: the 22 rule-only components' constants, disabled by default");

    bool camel = true;
    for (const auto& row : rows) {
        const std::string snake = optimaru::python::pythonName(row.name);
        camel = camel && !row.name.empty() && std::islower(static_cast<unsigned char>(row.name[0])) &&
                row.name.find(' ') == std::string::npos && snake.size() > 5 &&
                snake.compare(snake.size() - 5, 5, "_cost") == 0;
    }
    check(camel, "every registry name is camelCase and its Python name ends in _cost");
    check(optimaru::python::pythonName("maxConsecutiveDaysOffCost") == "max_consecutive_days_off_cost" &&
                  optimaru::python::pythonName("workerDayCountInRangeCost") ==
                          "worker_day_count_in_range_cost",
          "pythonName maps the registry names to their snake_case Python names");

    check(throwsInvalid([] { defaultConstraints().hard("desiredDayCost"); }),
          "hard() on the reward desiredDayCost throws");
    check(throwsInvalid([] { catalogConstraints().hard("desiredShiftCost"); }),
          "hard() on the reward desiredShiftCost throws");
    DesiredDayCost desired;
    desired.setLevel(optimaru::Level::Soft);
    check(desired.level == optimaru::Level::Soft, "re-stating a reward's soft level is a no-op");
    check(!throwsInvalid([] { defaultConstraints().soft("requiredSkillCost").hard("undesiredDayCost"); }),
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
        EmployeeSchedule copy = s;
        const Score monolithic = ScoreCalculator::calculateScore(copy);
        DecomposedCalculator calc;
        addDefaultConstraints(calc);
        const Score plugin = score(calc, s);
        if (!(monolithic.hard == plugin.hard && monolithic.soft == plugin.soft)) {
            if (++mismatches <= 3)
                std::printf("    round %d: monolith %s plugin %s\n", round, str(monolithic).c_str(),
                            str(plugin).c_str());
        }
    }
    check(mismatches == 0, "300 random assignments: the default assembly equals the monolith bit-for-bit");
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
    calc.emplace<WorkerPairingCost>(1.0, 0, 1, std::vector<int>{0, 1, 2, 3},
                                    std::vector<std::string>{"Location A", "Location A", "Location B",
                                                             "Location B"},
                                    false);
    calc.add(std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 2),
                                          std::vector<int>{0, 2}, std::vector<int>{}, true, false));
    calc.add(std::make_unique<ScopedUnit>(std::make_unique<UndesiredDayCost>(),
                                          std::vector<int>{}, std::vector<int>{0, 2, 4, 6, 8}, false, true));
}

void verifyIncrementalEqualsFull() {
    std::printf("== (c) incremental == full recompute, every component ==\n");
    EmployeeSchedule s = buildRoster(5);
    std::mt19937_64 rng(29);
    randomAssignment(s, rng, /*allowUnassigned=*/true);
    ScoreCalculator::prepare(s);
    DecomposedCalculator calc;
    addEveryComponent(calc);
    optimaru::ScoreDirector<EmployeeSchedule> director(s, calc);
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
            director.changeVariable(shift, b);
            director.changeVariable(other, a);
        } else {
            director.changeVariable(shift, static_cast<int>(rng() % (employees + 1)) - 1);
        }
        const Score incremental = director.calculateScore();
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
    ScopedUnit reward(std::make_unique<DesiredDayCost>(2.5), std::vector<int>{0}, std::vector<int>{}, true,
                      false);
    check(reward.name == "desiredDayCost" && reward.reward && !reward.levelFlippable &&
                  reward.weight == 2.5 && reward.level == optimaru::Level::Soft,
          "a scoped DesiredDayCost keeps name, reward, levelFlippable, weight and level");
    check(throwsInvalid([&] { reward.setLevel(optimaru::Level::Hard); }),
          "a scoped reward refuses setLevel(Hard)");
    ScopedUnit pattern(std::make_unique<DayPatternCost>(1.0, "OFF,Location A"), std::vector<int>{},
                       std::vector<int>{1, 2}, false, true);
    check(pattern.name == "dayPatternCost" && pattern.label == "Day pattern[OFF,Location A]" &&
                  pattern.levelFlippable && !pattern.reward,
          "a scoped DayPatternCost keeps its label and stays flippable");
    pattern.setLevel(optimaru::Level::Hard);
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
    check(RestAfterLocationRunCost(1.0, "Location B", 3).label == "Rest after location run[Location B 3]",
          "RestAfterLocationRunCost label");
    check(LocationRunGapCost(1.0, "", 10).label == "Location run gap[work 10]", "LocationRunGapCost label");
    check(WorkerLocationCountCost(1.0, "Location B", "Skill B", 4, 8).label ==
                  "Worker location count[Location B Skill B 4..8]",
          "WorkerLocationCountCost label");
    check(DayPatternCost(1.0, "Location A,OFF,Location B").label == "Day pattern[Location A,OFF,Location B]",
          "DayPatternCost label");
    check(TargetDaysOffCost(1.0, {5, 6}, 1).label == "Target days off[>=1]", "TargetDaysOffCost label");
    check(WorkerDayCountInRangeCost(1.0, 0, 6, 2, -1).label == "Worker day count[work 0..6 2..inf]",
          "WorkerDayCountInRangeCost label");
    check(LocationImbalanceCost(1.0, "", "Skill A", "gap", 1, {5}).label == "Balance[off-target Skill A gap]" &&
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
    const auto totals = calc.constraintTotals();
    const auto costs = calc.constraintCosts();
    check(totals.size() == 2 && totals[0].name == "dayPatternCost" && totals[1].name == "dayPatternCost" &&
                  totals[0].label == "Day pattern[WORK,WORK]" && totals[1].label == "Day pattern[OFF,OFF]" &&
                  costs.size() == 2 && costs[1].label == "Day pattern[OFF,OFF]",
          "two instances of one registry name report the plain name and their own labels");
}

}  // namespace

int main() {
    verifyCatalog();
    verifyDefaultAssemblyEqualsMonolith();
    verifyIncrementalEqualsFull();
    verifyScopedUnit();
    verifyLabels();
    if (failures == 0) {
        std::printf("All plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d plugin check(s) FAILED.\n", failures);
    return 1;
}
