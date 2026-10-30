#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "es/domain.hpp"
#include "es/incremental_calculator.hpp"
#include "es/score_calculator.hpp"
#include "es/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace es;

namespace {

// Minutes from the day-0 reference. dt(0, 8, 0) == day 0 at 08:00.
Minutes dt(int day, int hour, int minute) {
    return static_cast<Minutes>(day) * MINUTES_PER_DAY + hour * 60 + minute;
}

int failures = 0;

void check(const std::string& label, long actualHard, double actualSoft,
           long expectedHard, double expectedSoft) {
    // Soft compared with a small tolerance (it carries a rounded decimal).
    bool ok = (actualHard == expectedHard && std::fabs(actualSoft - expectedSoft) < 1e-6);
    std::printf("  [%s] %-34s got=%ldhard/%gsoft expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL", label.c_str(), actualHard, actualSoft,
                expectedHard, expectedSoft);
    if (!ok) ++failures;
}

// One employee with skill "X", available, assigned to every shift; each test
// isolates a single constraint by controlling the shift/employee data.
EmployeeSchedule oneEmployee(std::set<std::string> skills = {"X"}) {
    EmployeeSchedule s;
    s.employees.push_back(Employee{"Worker 01", std::move(skills), {}, {}, {}, {}});
    return s;
}

Shift makeShift(const std::string& id, Minutes start, Minutes end,
                const std::string& skill = "X") {
    Shift shift;
    shift.id = id;
    shift.start = start;
    shift.end = end;
    shift.requiredSkill = skill;
    shift.employeeIndex = 0;
    return shift;
}

void verifyConstraints() {
    std::printf("== Verifying each constraint in isolation ==\n");

    // 1) Missing required skill: employee has no skills -> ONE_HARD.
    {
        EmployeeSchedule s = oneEmployee(/*skills=*/{});
        s.shifts.push_back(makeShift("case 1.0", dt(0, 9, 0), dt(0, 17, 0), "X"));
        Score sc = ScoreCalculator::calculateScore(s);
        check("required skill", sc.hard, sc.soft, -1, 0);
    }

    // 2) Overlapping shifts: 11:00-12:00 overlap = 60 min. Same day also trips
    //    oneShiftPerDay (-1). Total hard = -(60 + 1).
    {
        EmployeeSchedule s = oneEmployee();
        s.shifts.push_back(makeShift("case 2.1", dt(0, 8, 0), dt(0, 12, 0)));
        s.shifts.push_back(makeShift("case 2.2", dt(0, 11, 0), dt(0, 13, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        check("overlap (+oneShiftPerDay)", sc.hard, sc.soft, -(60 + 1), 0);
    }

    // 3) Max one shift per day, non-overlapping same-day pair -> -1 only.
    {
        EmployeeSchedule s = oneEmployee();
        s.shifts.push_back(makeShift("case 3.1", dt(0, 8, 0), dt(0, 10, 0)));
        s.shifts.push_back(makeShift("case 3.2", dt(0, 12, 0), dt(0, 14, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        // gap = 12:00 - 10:00 = 120 min < 600 -> 10h penalty (600-120)=480,
        // plus oneShiftPerDay -1.
        check("oneShiftPerDay + 10h(sameday)", sc.hard, sc.soft, -(1 + 480), 0);
    }

    // 4) At least 10 hours between two shifts on different days.
    //    gap = day1 03:00 - day0 22:00 = 300 min -> penalty 600-300 = 300.
    {
        EmployeeSchedule s = oneEmployee();
        s.shifts.push_back(makeShift("case 4.1", dt(0, 20, 0), dt(0, 22, 0)));
        s.shifts.push_back(makeShift("case 4.2", dt(1, 3, 0), dt(1, 5, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        check("at least 10h between shifts", sc.hard, sc.soft, -300, 0);
    }

    // 5) Unavailable employee: 8h shift on an unavailable day -> hard -480.
    {
        EmployeeSchedule s = oneEmployee();
        s.employees[0].unavailableDates = {0};
        s.shifts.push_back(makeShift("case 5.0", dt(0, 9, 0), dt(0, 17, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        check("unavailable employee", sc.hard, sc.soft, -480, 0);
    }

    // 6) Undesired day: soft penalty by overlapping minutes.
    {
        EmployeeSchedule s = oneEmployee();
        s.employees[0].undesiredDates = {0};
        s.shifts.push_back(makeShift("case 6.0", dt(0, 9, 0), dt(0, 17, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        check("undesired day (soft)", sc.hard, sc.soft, 0, -480);
    }

    // 7) Desired day: soft reward by overlapping minutes.
    {
        EmployeeSchedule s = oneEmployee();
        s.employees[0].desiredDates = {0};
        s.shifts.push_back(makeShift("case 7.0", dt(0, 9, 0), dt(0, 17, 0)));
        Score sc = ScoreCalculator::calculateScore(s);
        check("desired day (soft reward)", sc.hard, sc.soft, 0, +480);
    }

    // 8) At least 10h between shifts, gap == 0 (back-to-back across midnight).
    //    A zero gap still counts as a gap, so it is penalized 600 - 0. Different
    //    start days => no oneShiftPerDay; no overlap.
    {
        EmployeeSchedule s = oneEmployee();
        s.shifts.push_back(makeShift("case 8.1", dt(0, 16, 0), dt(1, 0, 0)));  // ends midnight
        s.shifts.push_back(makeShift("case 8.2", dt(1, 0, 0), dt(1, 8, 0)));   // starts midnight
        Score sc = ScoreCalculator::calculateScore(s);
        check("10h between, gap=0 (<=)", sc.hard, sc.soft, -600, 0);
    }

    // 9) Load balancing: 3 employees, shift counts [2,1,1] on separate far-apart
    //    days (no other penalties). unfairness = sqrt(6 - 16/3) = sqrt(0.66667).
    {
        EmployeeSchedule s;
        for (const char* n : {"Worker 01", "Worker 02", "Worker 03"})
            s.employees.push_back(Employee{n, {"X"}, {}, {}, {}, {}});
        auto add = [&](const char* id, int day, int emp) {
            Shift sh = makeShift(id, dt(day, 8, 0), dt(day, 12, 0));
            sh.employeeIndex = emp;
            s.shifts.push_back(sh);
        };
        add("case 9.0", 0, 0); add("case 9.1", 3, 0);   // first worker: 2 shifts (days 0, 3)
        add("case 9.2", 6, 1);  // second worker: 1 shift
        add("case 9.3", 9, 2);  // third worker: 1 shift
        Score sc = ScoreCalculator::calculateScore(s);
        // sqrt(6 - 16/3) = sqrt(0.666667) = 0.81649658 -> 6 sig figs -> 0.816497
        check("load balance [2,1,1]", sc.hard, sc.soft, 0, -0.816497);
    }

    // 10) Load balancing with a zero-shift employee (complement): counts [2,0,0]
    //    over 3 employees => unfairness = sqrt(4 - 4/3) = sqrt(2.66667).
    {
        EmployeeSchedule s;
        for (const char* n : {"Worker 01", "Worker 02", "Worker 03"})
            s.employees.push_back(Employee{n, {"X"}, {}, {}, {}, {}});
        auto add = [&](const char* id, int day, int emp) {
            Shift sh = makeShift(id, dt(day, 8, 0), dt(day, 12, 0));
            sh.employeeIndex = emp;
            s.shifts.push_back(sh);
        };
        add("case 10.0", 0, 0); add("case 10.1", 3, 0);   // first worker: 2 shifts; the others: 0
        Score sc = ScoreCalculator::calculateScore(s);
        // sqrt(4 - 4/3) = sqrt(2.666667) = 1.63299316 -> 6 sig figs -> 1.63299
        check("load balance [2,0,0] (complement)", sc.hard, sc.soft, 0, -1.63299);
    }

    std::printf("\n");
}

// -------------------------------------------------------------------------
// A small synthetic instance to exercise the solver end to end.
// -------------------------------------------------------------------------
EmployeeSchedule buildDemo(int days, int shiftsPerDay, std::uint64_t seed) {
    EmployeeSchedule s;
    const std::vector<std::string> skills = {"Skill A", "Skill B", "Skill C"};

    // Six employees with overlapping but distinct skill sets and a few
    // unavailable / desired days. Worker 06 holds every skill, which gives the
    // roster enough slack to stay feasible on days when others are unavailable.
    s.employees = {
        Employee{"Worker 01", {"Skill A", "Skill B"}, {2}, {}, {0}, {}},
        Employee{"Worker 02", {"Skill A"}, {}, {3}, {1}, {}},
        Employee{"Worker 03", {"Skill B", "Skill C"}, {1}, {}, {}, {}},
        Employee{"Worker 04", {"Skill C", "Skill A"}, {}, {0}, {4}, {}},
        Employee{"Worker 05", {"Skill A", "Skill B", "Skill C"}, {4}, {}, {}, {}},
        Employee{"Worker 06", {"Skill A", "Skill B", "Skill C"}, {}, {}, {}, {}},
    };

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> skillPick(0, static_cast<int>(skills.size()) - 1);
    int id = 0;
    for (int d = 0; d < days; ++d) {
        for (int k = 0; k < shiftsPerDay; ++k) {
            // Morning (07-15) or evening (15-23) shift.
            Minutes start = dt(d, (k % 2 == 0) ? 7 : 15, 0);
            Minutes end = start + 8 * 60;
            s.shifts.push_back(
                makeShift("Shift " + std::to_string(id++), start, end, skills[skillPick(rng)]));
        }
    }
    // Start every shift unassigned.
    for (Shift& shift : s.shifts) shift.employeeIndex = -1;
    return s;
}

void solveDemo(int days, int shiftsPerDay, int seconds) {
    EmployeeSchedule schedule = buildDemo(days, shiftsPerDay, /*seed=*/42);
    std::printf("== Solving a demo roster (%d days x %d shifts/day = %d shifts, %zu employees, %ds) ==\n",
                days, shiftsPerDay, days * shiftsPerDay, schedule.employees.size(), seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score& sc) {
        std::printf("    new best @step %-8lld %s\n", static_cast<long long>(step),
                    sc.toString().c_str());
    };
    Solver solver(config);
    solver.solve(schedule);

    std::printf("  final score: %s  (feasible=%s)\n", schedule.score.toString().c_str(),
                schedule.score.isFeasible() ? "true" : "false");

    std::vector<int> load(schedule.employees.size(), 0);
    for (const Shift& shift : schedule.shifts)
        if (shift.employeeIndex >= 0) ++load[shift.employeeIndex];
    for (std::size_t e = 0; e < schedule.employees.size(); ++e)
        std::printf("    %-10s: %d shifts\n", schedule.employees[e].name.c_str(), load[e]);
    std::printf("\n");
}

}  // namespace

// -------------------------------------------------------------------------
// Verifies the incremental calculator stays exactly in sync with a full
// recompute across a long random walk of change/swap moves, driven through
// the engine's ScoreDirector protocol (before/set/after).
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== Verifying incremental scoring == full recompute ==\n");
    EmployeeSchedule s = buildDemo(/*days*/ 6, /*shiftsPerDay*/ 4, /*seed*/ 7);
    const int employeeCount = static_cast<int>(s.employees.size());
    for (int i = 0; i < static_cast<int>(s.shifts.size()); ++i) s.shifts[i].employeeIndex = i % employeeCount;

    EmployeeSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<EmployeeSchedule> director(s, calc);
    EmployeeSchedule shadow = s;
    std::mt19937_64 rng(123);
    std::uniform_int_distribution<int> shiftPick(0, static_cast<int>(s.shifts.size()) - 1);
    std::uniform_int_distribution<int> empPick(0, employeeCount - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 50000;
    for (int step = 0; step < steps; ++step) {
        if (unit(rng) < 0.75) {
            director.changeVariable(shiftPick(rng), empPick(rng));  // ChangeMove
        } else {
            int s1 = shiftPick(rng), s2 = shiftPick(rng);           // SwapMove
            int v1 = s.shifts[s1].employeeIndex, v2 = s.shifts[s2].employeeIndex;
            director.changeVariable(s1, v2);
            director.changeVariable(s2, v1);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < static_cast<int>(shadow.shifts.size()); ++i)
            shadow.shifts[i].employeeIndex = s.shifts[i].employeeIndex;
        Score full = ScoreCalculator::calculateScore(shadow);
        Score incremental = director.calculateScore();
        if (full.hard != incremental.hard || std::fabs(full.soft - incremental.soft) > 1e-9) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.toString().c_str(), full.toString().c_str());
            }
        }
    }
    if (mismatches == 0) {
        std::printf("  [PASS] %d random moves, incremental matched full recompute exactly\n\n",
                    steps);
    } else {
        std::printf("  [FAIL] %d / %d steps mismatched\n\n", mismatches, steps);
        ++failures;
    }

    // Force the memory-safe large-instance fallback: no quadratic
    // pair table, but the same exact marginal score calculation.
    EmployeeSchedule fallbackSchedule = s;
    EmployeeSchedule fallbackShadow = s;
    EmployeeSchedulingIncrementalScoreCalculator fallbackCalc(/*pairCacheBudgetBytes=*/0);
    optimaru::ScoreDirector<EmployeeSchedule> fallbackDirector(fallbackSchedule, fallbackCalc);
    std::mt19937_64 fallbackRng(456);
    int fallbackMismatches = 0;
    for (int step = 0; step < 5000; ++step) {
        const int shift = static_cast<int>(fallbackRng() % fallbackSchedule.shifts.size());
        const int employee = static_cast<int>(fallbackRng() % employeeCount);
        fallbackDirector.changeVariable(shift, employee);
        for (int i = 0; i < static_cast<int>(fallbackShadow.shifts.size()); ++i) {
            fallbackShadow.shifts[i].employeeIndex = fallbackSchedule.shifts[i].employeeIndex;
        }
        const Score full = ScoreCalculator::calculateScore(fallbackShadow);
        const Score incremental = fallbackDirector.calculateScore();
        if (full.hard != incremental.hard || std::fabs(full.soft - incremental.soft) > 1e-9) {
            ++fallbackMismatches;
        }
    }
    if (!fallbackCalc.usesPairPenaltyCache() && fallbackMismatches == 0) {
        std::printf("  [PASS] pair-cache memory fallback matched full recompute exactly\n\n");
    } else {
        std::printf("  [FAIL] pair-cache fallback mismatches=%d cacheUsed=%s\n\n",
                    fallbackMismatches, fallbackCalc.usesPairPenaltyCache() ? "true" : "false");
        ++failures;
    }
}

int main(int argc, char** argv) {
    int days = (argc > 1) ? std::atoi(argv[1]) : 5;
    int shiftsPerDay = (argc > 2) ? std::atoi(argv[2]) : 4;
    int seconds = (argc > 3) ? std::atoi(argv[3]) : 3;

    verifyConstraints();
    verifyIncremental();
    solveDemo(days, shiftsPerDay, seconds);

    if (failures == 0) {
        std::printf("All constraint checks PASSED.\n");
        return 0;
    }
    std::printf("%d constraint check(s) FAILED.\n", failures);
    return 1;
}
