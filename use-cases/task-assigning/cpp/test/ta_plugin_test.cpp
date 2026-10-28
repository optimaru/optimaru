// Plugin-assembly (ta/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random list
//       layouts (unassigned tasks included)
//   (b) incremental score == full recompute across 3000 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(solution, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "ta/constraints.hpp"
#include "ta/domain.hpp"
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

// One customer, one task type per added task (the ta_test builder).
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
};

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

// Scores the current lists with a freshly reset assembly, decoded back into
// the domain's Bendable levels.
Score scoreWith(DecomposedCalculator &calculator, TaskAssigningSolution &s)
{
    calculator.resetWorkingSolution(s);
    return fromEngineScore(calculator.calculateScore());
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute, on random list
// layouts (some tasks deliberately left out of every list).
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 30, /*employeeCount*/ 6, 7);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int taskCount = static_cast<int>(solution.tasks.size());
    const int employeeCount = static_cast<int>(solution.employees.size());
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round)
    {
        for (Employee &employee : solution.employees)
            employee.tasks.clear();
        for (int t = 0; t < taskCount; ++t)
        {
            if (unit(rng) < 0.10)
                continue;  // left unassigned
            const int list = static_cast<int>(rng() % employeeCount);
            solution.employees[list].tasks.push_back(t);
        }
        const Score plugin = scoreWith(calculator, solution);
        const Score monolithic = ScoreCalculator::calculateScore(solution);
        if (!(plugin == monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random list layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector so the plugin path's
// before/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 30, /*employeeCount*/ 6, 7);
    const int taskCount = static_cast<int>(solution.tasks.size());
    const int employeeCount = static_cast<int>(solution.employees.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<TaskAssigningSolution> director(solution,
                                                            calculator);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<int> taskList(taskCount, -1);  // which list holds each task
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
// (c) Single-component assemblies on hand-computed instances.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // noMissingSkills alone: the employee has none of the two required
        // skills -> 2 points on the Bendable hard level.
        Builder b;
        b.addEmployee({});
        const int task = b.addTask(/*baseDuration*/ 10, {"s0", "s1"});
        b.solution.employees[0].tasks.push_back(task);
        DecomposedCalculator calculator;
        calculator.emplace<NoMissingSkills>();
        checkScore("noMissingSkills alone",
                   scoreWith(calculator, b.solution),
                   Score{-2, 0, 0, 0});
    }
    {  // minimizeUnassignedTasks alone: two of three tasks left out.
        Builder b;
        b.addEmployee({});
        const int first = b.addTask(10);
        b.addTask(10);
        b.addTask(10);
        b.solution.employees[0].tasks.push_back(first);
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeUnassignedTasks>();
        checkScore("minimizeUnassignedTasks alone",
                   scoreWith(calculator, b.solution),
                   Score{0, -2, 0, 0});
    }
    {  // minimizeMakespan alone: one 10-minute task at affinity NONE (x 4)
        // ends at 40 -> 40^2 = 1600 on soft[1].
        Builder b;
        b.addEmployee({});
        const int task = b.addTask(10);
        b.solution.employees[0].tasks.push_back(task);
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeMakespan>();
        checkScore("minimizeMakespan alone",
                   scoreWith(calculator, b.solution),
                   Score{0, 0, -1600, 0});
    }
    {  // The three priority components split one chain by priority:
        // durations 10/10/10 at x 4 end at 40, 80, 120.
        Builder b;
        b.addEmployee({});
        const int critical = b.addTask(10, {}, Priority::CRITICAL);
        const int major = b.addTask(10, {}, Priority::MAJOR);
        const int minor = b.addTask(10, {}, Priority::MINOR);
        b.solution.employees[0].tasks = {critical, major, minor};

        DecomposedCalculator criticalOnly;
        criticalOnly.emplace<CriticalPriorityTaskEndTime>();
        checkScore("criticalPriorityTaskEndTime alone (4 * 40)",
                   scoreWith(criticalOnly, b.solution),
                   Score{0, 0, 0, -160});

        DecomposedCalculator majorOnly;
        majorOnly.emplace<MajorPriorityTaskEndTime>();
        checkScore("majorPriorityTaskEndTime alone (2 * 80)",
                   scoreWith(majorOnly, b.solution),
                   Score{0, 0, 0, -160});

        DecomposedCalculator minorOnly;
        minorOnly.emplace<MinorPriorityTaskEndTime>();
        checkScore("minorPriorityTaskEndTime alone (1 * 120)",
                   scoreWith(minorOnly, b.solution),
                   Score{0, 0, 0, -120});
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and the weight are assembly-time
// metadata, not part of the cost.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    Builder b;
    b.addEmployee({});
    const int task = b.addTask(/*baseDuration*/ 10, {"s0", "s1"});
    b.solution.employees[0].tasks.push_back(task);

    {
        // Re-priced onto the plain hard unit: 2 missing skills -> -2 in the
        // engine's hard level, which decodes as soft[0].
        DecomposedCalculator calculator;
        calculator.emplace<NoMissingSkills>(1.0);
        checkScore("noMissingSkills re-weighted to 1",
                   scoreWith(calculator, b.solution),
                   Score{0, -2, 0, 0});
    }
    {
        // Demoted to soft at weight 1: the same 2 points land in soft[2].
        DecomposedCalculator calculator;
        auto component = std::make_unique<NoMissingSkills>(1.0);
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("noMissingSkills demoted to soft",
                   scoreWith(calculator, b.solution),
                   Score{0, 0, 0, -2});
    }
    {
        // exponent 2 shapes the raw cost (2 missing skills) before the
        // weight: 4 points at weight 1.
        DecomposedCalculator calculator;
        auto component = std::make_unique<NoMissingSkills>(1.0);
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("noMissingSkills with exponent 2",
                   scoreWith(calculator, b.solution),
                   Score{0, -4, 0, 0});
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up to the score
// the same assembly reports.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 24, /*employeeCount*/ 5, 13);
    std::mt19937_64 rng(5);
    for (int t = 0; t < static_cast<int>(solution.tasks.size()); ++t)
    {
        const int list = static_cast<int>(rng() % solution.employees.size());
        solution.employees[list].tasks.push_back(t);
    }

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(solution);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 6,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path: the engine cross-checks the
// incremental score against a full reset + recompute after every move.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    TaskAssigningSolution solution
        = buildRandomInstance(/*taskCount*/ 20, /*employeeCount*/ 5, 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(solution, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(solution);
        check(solution.score == recomputed,
              "plugin-reported score == monolithic full recompute");
        check(solution.score.soft0 == 0, "every task assigned after the solve");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All task-assigning plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d task-assigning plugin check(s) FAILED.\n", failures);
    return 1;
}
