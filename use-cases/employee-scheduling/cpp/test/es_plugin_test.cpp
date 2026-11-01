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
//   (f) parallel workers: a throwing onNewBest (worker 0) stops the other
//       workers, so a 30 s, 4-worker solve rethrows at once (Python reaches
//       no thread pool here: solve_request forks processes for its starts)
//   (h) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (undesired minutes + 0.5 x undesired minutes^2), threshold and
//       literal offsets by hand (literal mode charges every employee, a
//       worker scope's employees only), the literal formula refused on hard
//       at reset (a scoped one included) and kept on soft, incremental == a
//       fresh recompute over a random walk and a FULL_ASSERT solve with
//       repeated, shifted components (delta-grouped hard, shared-context day
//       patterns, scoped and global ones)
// The hand-calculated positive and zero raw cost of every constraint is
// checked through the bindings (python/tests/test_constraint_costs.py).
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
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/impl/solver/worker_threads.hpp"
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

// ---------------------------------------------------------------------------
// (f) a failing parallel worker stops the others
// ---------------------------------------------------------------------------
void verifyParallelFailureStopsWorkers() {
    std::printf("== (f) a failing parallel worker stops the others ==\n");
    EmployeeSchedule s = buildRoster(5);
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(30);
    config.parallelSolverCount = 4;
    config.onNewBest = [](std::int64_t, const Score&) { throw std::runtime_error("boom"); };
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
    config.spentLimit = std::chrono::seconds(30);
    config.parallelSolverCount = 4;
    // std::thread throws std::system_error when the system has no thread to
    // give; here worker 1's creation fails through the engine's test seam.
    optimaru::workerThreadStartHook = [](int index) {
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
    optimaru::workerThreadStartHook = nullptr;
    const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    check(raised && seconds < 10.0,
          "worker 1 cannot start: worker 0 is stopped and joined, the error reaches the caller "
          "after " + std::to_string(seconds) + " s (no std::terminate)");
}

// ---------------------------------------------------------------------------
// (h) repeated components and offsets
// ---------------------------------------------------------------------------
// The bindings build every pick as its own component (the factory), so a
// repeat is simply a second component of the same type, and the knobs are
// re-stated on it (applyKnobs: setWeight / setExponent / setOffset / setClip).
template <typename C, typename... Args>
std::unique_ptr<C> shaped(double offset, bool clip, Args&&... args) {
    auto component = std::make_unique<C>(std::forward<Args>(args)...);
    component->setOffset(offset);
    component->setClip(clip);
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
    calc.add(shaped<RequiredSkillCost>(1.0, true, 2.0));             // delta-grouped hard, threshold
    auto literalSkill = shaped<RequiredSkillCost>(1.0, false, 2.0);  // ... and a soft literal copy
    literalSkill->setLevel(optimaru::Level::Soft);
    calc.add(std::move(literalSkill));
    calc.add(shaped<EmployeeUnavailabilityCost>(60.0, true));        // hard threshold
    auto squared = shaped<UndesiredDayCost>(120.0, true, 0.01);      // a repeat: (minutes - 120)^2
    squared->setExponent(2.0);
    calc.add(std::move(squared));
    calc.add(shaped<UndesiredDayCost>(-30.0, false, 0.5));           // ... and a literal one
    calc.add(shaped<DesiredDayCost>(60.0, false));                   // a reward, literal
    calc.add(shaped<DayPatternCost>(-1.0, true, 1.0, "WORK,WORK,WORK"));  // shared day-pattern context
    calc.add(shaped<ShiftCountImbalanceCost>(0.5, false, 2.0));      // global
    calc.add(shaped<LocationImbalanceCost>(1.0, false, 1.0, "Location A", "", "ssd", 0, std::vector<int>{}));
    auto scoped = std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 3),
                                               std::vector<int>{1, 3}, std::vector<int>{}, true, false);
    scoped->setOffset(2.0);
    scoped->setClip(false);
    scoped->setLevel(optimaru::Level::Soft);  // a scoped literal copy, on soft
    calc.add(std::move(scoped));
    // Repeats on another level than their default: a hard one soft, a soft one hard.
    auto demoted = shaped<EmployeeUnavailabilityCost>(30.0, false, 0.5);
    demoted->setLevel(optimaru::Level::Soft);
    calc.add(std::move(demoted));
    auto promoted = shaped<UndesiredDayCost>(240.0, true);
    promoted->setLevel(optimaru::Level::Hard);
    calc.add(std::move(promoted));
}

void verifyRepeatsAndOffsets() {
    std::printf("== (h) repeated components, offset / clip ==\n");
    {
        EmployeeSchedule s = repeatInstance();
        DecomposedCalculator calc;
        calc.emplace<UndesiredDayCost>();
        auto second = std::make_unique<UndesiredDayCost>(0.5);
        second->setExponent(2.0);
        calc.add(std::move(second));
        const Score result = score(calc, s);
        check(result.hard == 0 && result.soft == -(480.0 + 0.5 * 480.0 * 480.0),
              "undesired + 0.5 x undesired^2: -(480 + 115200) soft");
        const auto entries = calc.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts &&
                      entries[0].unitCosts == std::vector<double>{480.0, 0.0} && entries[1].weight == 0.5 &&
                      entries[1].exponent == 2.0 && entries[0].name == entries[1].name,
              "two instances, the same raw costs, each with its own knobs");
    }
    {
        EmployeeSchedule s = repeatInstance();
        auto soft = [&](std::unique_ptr<EsUnitConstraint> component) {
            DecomposedCalculator calc;
            calc.add(std::move(component));
            return score(calc, s);
        };
        check(soft(shaped<UndesiredDayCost>(100.0, true)).soft == -380.0, "threshold offset 100: 480 - 100");
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
        hard->setLevel(optimaru::Level::Hard);
        check(soft(std::move(hard)).hard == -2, "hard threshold offset 479.5 at weight 3: llround(1.5) = 2");
        // The literal unavailability pick on soft charges both employees,
        // 3 x (0 - 0.5) = -1.5 each, not rounded: +3 soft.
        auto literal = shaped<EmployeeUnavailabilityCost>(0.5, false, 3.0);
        literal->setLevel(optimaru::Level::Soft);
        const Score literalScore = soft(std::move(literal));
        check(literalScore.hard == 0 && literalScore.soft == 3.0,
              "soft literal offset 0.5 at weight 3 on unavailability: 2 x 1.5 = +3 soft");
        // A worker scope: only Worker 2 is evaluated, so the literal charges it alone.
        DecomposedCalculator scopedCalc;
        auto scoped = std::make_unique<ScopedUnit>(std::make_unique<UndesiredDayCost>(), std::vector<int>{1, 1, 7},
                                                   std::vector<int>{}, true, false);
        scoped->setOffset(100.0);
        scoped->setClip(false);
        scopedCalc.add(std::move(scoped));
        const Score scopedScore = score(scopedCalc, s);
        const auto scopedEntries = scopedCalc.constraintCosts();
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
                   "' cannot use clip=false with a non-zero offset on a level that counts toward the "
                   "hard score (hard 0 must mean no violation)";
        };
        auto clipFirst = std::make_unique<EmployeeUnavailabilityCost>(3.0);
        clipFirst->setClip(false);
        clipFirst->setOffset(-0.5);
        check(resetRefusal(shaped<EmployeeUnavailabilityCost>(0.5, false, 3.0)) ==
                              refusal("employeeUnavailabilityCost") &&
                      resetRefusal(std::move(clipFirst)) == refusal("employeeUnavailabilityCost"),
              "unavailability literal offset +-0.5 on hard: refused at reset");
        auto promoted = shaped<UndesiredDayCost>(100.0, false);
        promoted->setLevel(optimaru::Level::Hard);
        auto scoped = std::make_unique<ScopedUnit>(std::make_unique<MaxConsecutiveDaysCost>(1.0, 3),
                                                   std::vector<int>{1}, std::vector<int>{}, true, false);
        scoped->setOffset(2.0);
        scoped->setClip(false);
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
        softLiteral->setLevel(optimaru::Level::Soft);
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
        optimaru::ScoreDirector<EmployeeSchedule> director(s, calc);
        const int employees = static_cast<int>(s.employees.size());
        const int shifts = static_cast<int>(s.shifts.size());
        int mismatches = 0;
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
            addRepeatedShifted(fresh);
            const Score full = score(fresh, shadow);
            if (!optimaru::scoresAgree(incremental, full) && ++mismatches <= 3)
                std::printf("    step %d: incremental %s full %s\n", step, str(incremental).c_str(),
                            str(full).c_str());
        }
        check(mismatches == 0, std::to_string(steps) +
                                       " random moves with repeated, shifted components: incremental == fresh");
    }
    {
        EmployeeSchedule s = buildRoster(17);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 5;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        DecomposedCalculator calc;
        addRepeatedShifted(calc);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(s, calc);
        } catch (const optimaru::ScoreCorruptionError& e) {
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
