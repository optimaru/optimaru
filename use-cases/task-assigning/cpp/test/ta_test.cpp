// Task-assigning unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) every constraint verified in isolation against hand-computed scores
//       (the Java TaskAssigningConstraintProviderTest cases; where the Java
//       fixture set inconsistent explicit shadow values, the expectation is
//       re-derived with consistent chained start times — same formulas)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/mutate/after),
//       including unassigned tasks
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) the demo instance solves to hard 0 with every task assigned
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "ta/demo_data.hpp"
#include "ta/domain.hpp"
#include "ta/incremental_calculator.hpp"
#include "ta/score_calculator.hpp"
#include "ta/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace ta;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                const Score &expected)
{
    const bool ok = actual == expected;
    std::printf("  [%s] %-44s got=%s expected=%s\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expected.toString().c_str());
    if (!ok)
        ++failures;
}

// One customer, one task type per added task; affinity/skills as requested.
struct Builder
{
    TaskAssigningSolution solution;

    Builder() { solution.customers.push_back({"c0", "Customer 0"}); }

    int addCustomer()
    {
        solution.customers.push_back(
            {"c" + std::to_string(solution.customers.size()), ""});
        return static_cast<int>(solution.customers.size()) - 1;
    }

    int addEmployee(std::vector<std::string> skills,
                    std::vector<Affinity> affinities = {})
    {
        Employee employee;
        employee.id = "e" + std::to_string(solution.employees.size());
        employee.fullName = employee.id;
        employee.skills = std::move(skills);
        employee.affinityByCustomer = std::move(affinities);
        solution.employees.push_back(employee);
        return static_cast<int>(solution.employees.size()) - 1;
    }

    int addTask(Minutes baseDuration,
                std::vector<std::string> requiredSkills = {},
                Priority priority = Priority::MINOR,
                Minutes minStartTime = 0,
                int customer = 0)
    {
        TaskType type;
        type.code = "T" + std::to_string(solution.taskTypes.size());
        type.title = type.code;
        type.baseDuration = baseDuration;
        type.requiredSkills = std::move(requiredSkills);
        solution.taskTypes.push_back(type);

        Task task;
        task.id = std::to_string(solution.tasks.size() + 1);
        task.taskType = static_cast<int>(solution.taskTypes.size()) - 1;
        task.indexInTaskType = 1;
        task.customer = customer;
        task.minStartTime = minStartTime;
        task.priority = priority;
        solution.tasks.push_back(task);
        return static_cast<int>(solution.tasks.size()) - 1;
    }

    void assign(int employee, std::vector<int> tasks)
    {
        solution.employees[employee].tasks = std::move(tasks);
    }
};

// -------------------------------------------------------------------------
// (a) Each constraint in isolation, full recompute vs hand-computed scores.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Constraints vs hand-computed scores ==\n");

    {
        // Java noMissingSkills: employee has skill "3"; tasks require
        // {1,2} / {3} / {5} -> 3 missing skills in total.
        Builder b;
        const int e = b.addEmployee({"3"});
        const int t1 = b.addTask(1, {"1", "2"}, Priority::CRITICAL, 1);
        const int t2 = b.addTask(1, {"3"}, Priority::CRITICAL, 1);
        const int t3 = b.addTask(1, {"5"}, Priority::CRITICAL, 1);
        b.assign(e, {t1, t2, t3});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.hard == -3,
              "noMissingSkills penalizes 3 (got "
                  + std::to_string(-b.solution.score.hard) + ")");
    }
    {
        // Java minimizeUnassignedTasks: 3 tasks, 1 assigned -> penalty 2.
        Builder b;
        const int e = b.addEmployee({});
        b.addTask(1);
        b.addTask(1);
        const int t3 = b.addTask(1);
        b.assign(e, {t3});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.soft0 == -2,
              "minimizeUnassignedTasks penalizes 2 (got "
                  + std::to_string(-b.solution.score.soft0) + ")");
    }
    {
        // Java criticalPriorityTaskEndTime: HIGH affinity (x1), duration 10,
        // minStartTime 1 -> endTime 11, penalty 11 * 4 = 44. The given MINOR
        // task stays unassigned (it contributes nothing to soft[2]).
        Builder b;
        const int e = b.addEmployee({}, {Affinity::HIGH});
        const int t1 = b.addTask(10, {}, Priority::CRITICAL, 1);
        b.addTask(20, {}, Priority::MINOR, 2);
        b.assign(e, {t1});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.soft2 == -44,
              "criticalPriorityTaskEndTime penalizes 44 (got "
                  + std::to_string(-b.solution.score.soft2) + ")");
    }
    {
        // Java majorPriorityTaskEndTime: endTime 11, x2 = 22.
        Builder b;
        const int e = b.addEmployee({}, {Affinity::HIGH});
        const int t1 = b.addTask(10, {}, Priority::MAJOR, 1);
        b.addTask(20, {}, Priority::MINOR, 2);
        b.assign(e, {t1});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.soft2 == -22,
              "majorPriorityTaskEndTime penalizes 22 (got "
                  + std::to_string(-b.solution.score.soft2) + ")");
    }
    {
        // Java minorPriorityTaskEndTime: endTime 11, x1 = 11.
        Builder b;
        const int e = b.addEmployee({}, {Affinity::HIGH});
        const int t1 = b.addTask(10, {}, Priority::MINOR, 1);
        b.addTask(20, {}, Priority::MAJOR, 2);
        b.assign(e, {t1});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.soft2 == -11,
              "minorPriorityTaskEndTime penalizes 11 (got "
                  + std::to_string(-b.solution.score.soft2) + ")");
    }
    {
        // minimizeMakespan, re-derived with consistent chained start times
        // (the Java test fixed startTime shadows by hand):
        //   employee1 (MEDIUM = x2): task1 dur 10 minStart 1 -> start 1,
        //     end 21; task2 dur 20 minStart 2 -> start max(21,2)=21, end 61.
        //     endTime 61 -> 61^2 = 3721.
        //   employee2 (HIGH = x1): task3 dur 1 minStart 1 -> end 2 -> 4.
        //   total 3725.
        Builder b;
        const int e1 = b.addEmployee({}, {Affinity::MEDIUM});
        const int e2 = b.addEmployee({}, {Affinity::HIGH});
        const int t1 = b.addTask(10, {}, Priority::CRITICAL, 1);
        const int t2 = b.addTask(20, {}, Priority::MINOR, 2);
        const int t3 = b.addTask(1, {}, Priority::CRITICAL, 1);
        b.assign(e1, {t1, t2});
        b.assign(e2, {t3});
        ScoreCalculator::calculateScore(b.solution);
        check(b.solution.score.soft1 == -3725,
              "minimizeMakespan penalizes 61^2 + 2^2 = 3725 (got "
                  + std::to_string(-b.solution.score.soft1) + ")");
        // Shadow variables re-derived like the Java supplier chain.
        check(b.solution.tasks[t2].startTime == 21
                  && b.solution.tasks[t2].employee == e1,
              "chained startTime = max(previous end, minStartTime)");
    }
    {
        // Affinity duration multipliers: NONE x4, LOW x3, MEDIUM x2, HIGH x1
        // (the affinity map defaults to NONE for unknown customers).
        Builder b;
        const int c1 = b.addCustomer();
        const int e = b.addEmployee({}, {Affinity::LOW});
        const int t1 = b.addTask(10, {}, Priority::MINOR, 0, 0);   // LOW x3
        const int t2 = b.addTask(10, {}, Priority::MINOR, 0, c1);  // NONE x4
        b.assign(e, {t1, t2});
        ScoreCalculator::calculateScore(b.solution);
        // t1: start 0, end 30. t2: start 30, end 70.
        check(b.solution.tasks[t1].startTime == 0
                  && b.solution.tasks[t2].startTime == 30
                  && b.solution.score.soft1 == -(70 * 70),
              "affinity multipliers LOW x3 / NONE x4 drive durations");
    }
    {
        // All four levels at once + the engine-score round trip.
        // employee (HIGH): task1 CRITICAL dur 10 minStart 1 (requires a
        // missing skill), task2 MINOR dur 20 minStart 2; task3 unassigned.
        //   hard: -1; soft0: -1; chain: t1 end 11, t2 start 11 end 31.
        //   soft1: -(31^2) = -961; soft2: -(11*4 + 31*1) = -75.
        Builder b;
        const int e = b.addEmployee({}, {Affinity::HIGH});
        const int t1 = b.addTask(10, {"missing"}, Priority::CRITICAL, 1);
        const int t2 = b.addTask(20, {}, Priority::MINOR, 2);
        b.addTask(5, {}, Priority::MAJOR, 0);
        b.assign(e, {t1, t2});
        const Score score = ScoreCalculator::calculateScore(b.solution);
        checkScore("all levels combined", score, {-1, -1, -961, -75});
        check(fromEngineScore(toEngineScore(score)) == score,
              "fromEngineScore(toEngineScore(s)) == s");
        check(!score.isFeasible() && Score{0, -5, -100, -10}.isFeasible(),
              "isFeasible depends on the hard level only");
        // Level priority survives the engine mapping.
        check(toEngineScore(Score{0, -1, 0, 0})
                      < toEngineScore(Score{-0, 0, -1000000, -999999})
                  && toEngineScore(Score{-1, 0, 0, 0})
                         < toEngineScore(Score{0, -100000, -1000000, -9}),
              "lexicographic order preserved by the engine mapping");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random instance for the incremental / solve tests.
// -------------------------------------------------------------------------
TaskAssigningSolution
buildRandomInstance(int taskCount, int employeeCount, unsigned seed)
{
    const std::vector<std::string> skillPool
        = {"s0", "s1", "s2", "s3", "s4", "s5"};
    std::mt19937_64 rng(seed);
    Builder b;
    b.addCustomer();
    b.addCustomer();
    b.addCustomer();  // 4 customers in total
    for (int e = 0; e < employeeCount; ++e)
    {
        std::vector<std::string> skills;
        for (const std::string &skill : skillPool)
        {
            if (rng() % 100 < 45)
                skills.push_back(skill);
        }
        std::vector<Affinity> affinities;
        for (int c = 0; c < 4; ++c)
            affinities.push_back(static_cast<Affinity>(rng() % 4));
        b.addEmployee(std::move(skills), std::move(affinities));
    }
    for (int t = 0; t < taskCount; ++t)
    {
        std::vector<std::string> required;
        for (const std::string &skill : skillPool)
        {
            if (rng() % 100 < 20)
                required.push_back(skill);
        }
        b.addTask(static_cast<Minutes>(10 + rng() % 51),
                  std::move(required),
                  static_cast<Priority>(rng() % 3),
                  static_cast<Minutes>(rng() % 120),
                  static_cast<int>(rng() % 4));
    }
    return b.solution;
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute across a random walk of LIST mutations
// through the ScoreDirector protocol.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Incremental vs full recompute (random walk) ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 30, /*employeeCount*/ 6, 7);
    const int taskCount = static_cast<int>(solution.tasks.size());
    const int employeeCount = static_cast<int>(solution.employees.size());

    TaskAssigningIncrementalScoreCalculator calculator;
    optimaru::ScoreDirector<TaskAssigningSolution> director(solution,
                                                            calculator);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // Track which list each task is in (-1 == unassigned).
    std::vector<int> taskList(taskCount, -1);
    std::vector<int> unassigned;
    for (int t = 0; t < taskCount; ++t)
        unassigned.push_back(t);

    auto indexWithin = [&](int list, int task)
    {
        const std::vector<int> &tasks = solution.employees[list].tasks;
        for (std::size_t p = 0; p < tasks.size(); ++p)
        {
            if (tasks[p] == task)
                return static_cast<int>(p);
        }
        return -1;
    };

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        const double dice = unit(rng);
        if (dice < 0.35 && !unassigned.empty())
        {
            // Assign an unassigned task to a random (list, position).
            const std::size_t pick = rng() % unassigned.size();
            const int task = unassigned[pick];
            unassigned.erase(unassigned.begin()
                             + static_cast<std::ptrdiff_t>(pick));
            const int list = static_cast<int>(rng() % employeeCount);
            const int position = static_cast<int>(
                rng() % (solution.employees[list].tasks.size() + 1));
            director.insertListElement(list, position, task);
            taskList[task] = list;
        }
        else if (dice < 0.55)
        {
            // Unassign a random assigned task.
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            director.removeListElement(taskList[task],
                                       indexWithin(taskList[task], task));
            taskList[task] = -1;
            unassigned.push_back(task);
        }
        else if (dice < 0.85)
        {
            // Relocate a random assigned task.
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            director.removeListElement(taskList[task],
                                       indexWithin(taskList[task], task));
            const int list = static_cast<int>(rng() % employeeCount);
            const int position = static_cast<int>(
                rng() % (solution.employees[list].tasks.size() + 1));
            director.insertListElement(list, position, task);
            taskList[task] = list;
        }
        else
        {
            // Reverse a random segment within a random list (2-opt).
            const int list = static_cast<int>(rng() % employeeCount);
            const int size
                = static_cast<int>(solution.employees[list].tasks.size());
            if (size < 2)
                continue;
            int first = static_cast<int>(rng() % size);
            int last = static_cast<int>(rng() % size);
            if (first > last)
                std::swap(first, last);
            director.reverseListSegment(list, first, last);
        }

        const optimaru::HardSoftScore incremental = director.calculateScore();
        // Full recompute on the same lists (only rewrites shadow fields the
        // incremental calculator never reads).
        const optimaru::HardSoftScore full
            = ScoreCalculator::calculateEngineScore(solution);
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 20, /*employeeCount*/ 5, 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score finalScore{};
    try
    {
        Solver solver(config);
        solver.solve(solution);
        finalScore = solution.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(solution);
        check(finalScore == recomputed,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) The demo instance solves to hard 0 with every task assigned exactly
// once (the demo data admits a zero-missing-skill assignment).
// -------------------------------------------------------------------------
void verifyFeasibleSolve()
{
    std::printf("== (d) Demo instance solve reaches hard 0 ==\n");
    TaskAssigningSolution solution = demo::generateDemoData();

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(solution);

    check(solution.score.hard == 0, "demo instance reaches [0]hard");
    check(solution.score.soft0 == 0, "no task left unassigned");
    std::vector<int> seen(solution.tasks.size(), 0);
    for (const Employee &employee : solution.employees)
    {
        for (int taskIndex : employee.tasks)
            ++seen[taskIndex];
    }
    bool eachOnce = true;
    for (int count : seen)
    {
        if (count != 1)
            eachOnce = false;
    }
    check(eachOnce, "every task appears in exactly one employee list");
    // Shadow variables are consistent on the returned best solution.
    bool shadowsConsistent = true;
    for (std::size_t e = 0; e < solution.employees.size(); ++e)
    {
        Minutes previousEnd = 0;
        bool first = true;
        for (int taskIndex : solution.employees[e].tasks)
        {
            const Task &task = solution.tasks[taskIndex];
            const Minutes expected
                = first ? task.minStartTime
                        : std::max(previousEnd, task.minStartTime);
            if (task.employee != static_cast<int>(e)
                || task.startTime != expected)
                shadowsConsistent = false;
            previousEnd
                = expected + solution.taskDuration(task, static_cast<int>(e));
            first = false;
        }
    }
    check(shadowsConsistent, "task shadow variables (employee, startTime)");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyFeasibleSolve();

    if (failures == 0)
    {
        std::printf("All task-assigning checks PASSED.\n");
        return 0;
    }
    std::printf("%d task-assigning check(s) FAILED.\n", failures);
    return 1;
}
