// Plugin-assembly (ta/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random list
//       layouts (unassigned tasks included)
//   (b) incremental score == full recompute across 3000 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level / exponent overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(solution, calculator) under FULL_ASSERT
//   (g) the medium level: the below-hard fold, hard() / soft() / weight on
//       unassignedTasksCost, and a random walk with the medium term
//       overridden (incremental == full recompute)
//   (h) each component's raw cost by hand: one positive, one zero case
//   (i) the catalog (defaultConstraints().info()) == the components'
//       constants, and the knob guards
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand (an idle employee is
//       an evaluated unit), the global's total shaped once, incremental == a
//       fresh recompute over a random LIST walk and a FULL_ASSERT solve with
//       repeated, shifted components
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
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

    {  // requiredSkillCost alone: the employee has none of the two required
        // skills -> 2 points on the Bendable hard level.
        Builder b;
        b.addEmployee({});
        const int task = b.addTask(/*baseDuration*/ 10, {"s0", "s1"});
        b.solution.employees[0].tasks.push_back(task);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkillCost>();
        checkScore("requiredSkillCost alone",
                   scoreWith(calculator, b.solution),
                   Score{-2, 0, 0, 0});
    }
    {  // unassignedTasksCost alone: two of three tasks left out.
        Builder b;
        b.addEmployee({});
        const int first = b.addTask(10);
        b.addTask(10);
        b.addTask(10);
        b.solution.employees[0].tasks.push_back(first);
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedTasksCost>();
        checkScore("unassignedTasksCost alone",
                   scoreWith(calculator, b.solution),
                   Score{0, -2, 0, 0});
    }
    {  // squaredMakespanCost alone: one 10-minute task at affinity NONE (x 4)
        // ends at 40 -> 40^2 = 1600 on soft[1].
        Builder b;
        b.addEmployee({});
        const int task = b.addTask(10);
        b.solution.employees[0].tasks.push_back(task);
        DecomposedCalculator calculator;
        calculator.emplace<SquaredMakespanCost>();
        checkScore("squaredMakespanCost alone",
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
        criticalOnly.emplace<CriticalPriorityTaskEndTimeCost>();
        checkScore("criticalPriorityTaskEndTimeCost alone (4 * 40)",
                   scoreWith(criticalOnly, b.solution),
                   Score{0, 0, 0, -160});

        DecomposedCalculator majorOnly;
        majorOnly.emplace<MajorPriorityTaskEndTimeCost>();
        checkScore("majorPriorityTaskEndTimeCost alone (2 * 80)",
                   scoreWith(majorOnly, b.solution),
                   Score{0, 0, 0, -160});

        DecomposedCalculator minorOnly;
        minorOnly.emplace<MinorPriorityTaskEndTimeCost>();
        checkScore("minorPriorityTaskEndTimeCost alone (1 * 120)",
                   scoreWith(minorOnly, b.solution),
                   Score{0, 0, 0, -120});
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and the weight are assembly-time
// metadata, not part of the cost. Every weight is in units of its level:
// the 10^9 between a missing skill and an unassigned task is the domain's
// medium fold, not part of requiredSkillCost's weight (0.2.0).
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    Builder b;
    b.addEmployee({});
    const int task = b.addTask(/*baseDuration*/ 10, {"s0", "s1"});
    b.solution.employees[0].tasks.push_back(task);

    {
        // Re-weighted to 3 hard units per missing skill: 2 missing skills
        // -> -6 on the Bendable hard level (the fold keeps the 10^9).
        DecomposedCalculator calculator;
        calculator.emplace<RequiredSkillCost>(3.0);
        checkScore("requiredSkillCost re-weighted to 3",
                   scoreWith(calculator, b.solution),
                   Score{-6, 0, 0, 0});
    }
    {
        // Demoted to soft at weight 1: the same 2 points land in soft[2].
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredSkillCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("requiredSkillCost demoted to soft",
                   scoreWith(calculator, b.solution),
                   Score{0, 0, 0, -2});
    }
    {
        // exponent 2 shapes the raw cost (2 missing skills) before the
        // weight: 4 hard points at weight 1.
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredSkillCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("requiredSkillCost with exponent 2",
                   scoreWith(calculator, b.solution),
                   Score{-4, 0, 0, 0});
    }
    {
        // A soft rule promoted to hard weighs its weight in HARD units: the
        // one task (10 min x NONE 4 = 40, MINOR) costs 1 * 40 on the hard
        // level, i.e. 40 * 10^9 engine-hard units.
        DecomposedCalculator calculator;
        defaultConstraints()
            .only({"minorPriorityTaskEndTimeCost"})
            .hard("minorPriorityTaskEndTimeCost")
            .into(calculator);
        checkScore("minorPriorityTaskEndTimeCost promoted to hard",
                   scoreWith(calculator, b.solution),
                   Score{-40, 0, 0, 0});
    }
    {
        // Named edits read the same as the components edited by hand.
        DecomposedCalculator byHand;
        {
            auto skills = std::make_unique<RequiredSkillCost>();
            skills->level = optimaru::Level::Soft;
            byHand.add(std::move(skills));
            byHand.emplace<CriticalPriorityTaskEndTimeCost>(8.0);
        }
        DecomposedCalculator byName;
        defaultConstraints()
            .only({"requiredSkillCost", "criticalPriorityTaskEndTimeCost"})
            .soft("requiredSkillCost")
            .weight("criticalPriorityTaskEndTimeCost", 8)
            .into(byName);
        checkScore("named edits == the same components by hand",
                   scoreWith(byName, b.solution),
                   scoreWith(byHand, b.solution));
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

    // Leave employee 0's tasks out so the medium total is non-zero too.
    solution.employees[0].tasks.clear();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(solution);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    // A medium total is in medium units (not multiplied by the fold), so it
    // is folded here the way the calculator folds its medium accumulator:
    // engine.hard = hard * kMissingSkillUnit + medium.
    double hard = 0.0;
    double medium = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            medium += total.cost;
        else
            soft += total.cost;
    }
    check(medium > 0.0, "the instance leaves tasks unassigned (medium > 0)");
    check(std::fabs(-(hard * static_cast<double>(kMissingSkillUnit) + medium)
                    - static_cast<double>(score.hard))
                  < 1e-3
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals (medium folded below hard) sum to the "
          "reported score");
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


// -------------------------------------------------------------------------
// (g) The medium level: unassignedTasksCost (Bendable soft[0]) folds below
// hard (engine.hard = hard * 10^9 + medium). Its weight is in medium units;
// hard() gives 1 hard unit per task, soft() 1 soft unit per task (outside
// the fold). A selection without any medium term still folds hard * 10^9.
// -------------------------------------------------------------------------
Score scoreOf(ConstraintAssembly assembly, TaskAssigningSolution &s)
{
    DecomposedCalculator calculator;
    assembly.into(calculator);
    return scoreWith(calculator, s);
}

void verifyMediumLevel()
{
    std::printf("== (g) Medium level ==\n");
    // One employee without skills: task 0 (needs s0, 10 min x NONE 4 -> ends
    // at 40, MINOR) is on its list; tasks 1 and 2 are unassigned.
    Builder b;
    b.addEmployee({});
    const int task = b.addTask(10, {"s0"});
    b.addTask(10);
    b.addTask(10);
    b.solution.employees[0].tasks.push_back(task);
    TaskAssigningSolution &s = b.solution;

    const Score monolithic = ScoreCalculator::calculateScore(s);
    checkScore("monolith: 1 missing skill, 2 unassigned, 40^2, 40",
               monolithic,
               Score{-1, -2, -1600, -40});
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.resetWorkingSolution(s);
        const optimaru::HardSoftScore engine = calculator.calculateScore();
        check(engine.hard == -(1 * kMissingSkillUnit + 2)
                  && engine.soft == -(1600.0 * 1e7 + 40.0),
              "default: engine.hard = hard * 10^9 + medium, soft = "
              "soft1 * 10^7 + soft2");
        const auto levels = calculator.levelScores();
        check(levels.hard == -1 && levels.medium == -2.0
                  && levels.soft == -(1600.0 * 1e7 + 40.0),
              "levelScores() reports the three levels unfolded");
        check(engine == toEngineScore(monolithic),
              "the decomposed fold == the monolith's Score adapter");
    }
    checkScore("unassignedTasksCost.hard(): 1 hard unit per task",
               scoreOf(defaultConstraints().hard("unassignedTasksCost"), s),
               Score{-3, 0, -1600, -40});
    checkScore("unassignedTasksCost.soft(): 1 soft unit per task",
               scoreOf(defaultConstraints().soft("unassignedTasksCost"), s),
               Score{-1, 0, -1600, -42});
    checkScore("unassignedTasksCost weight 3 stays in medium units",
               scoreOf(defaultConstraints().weight("unassignedTasksCost", 3), s),
               Score{-1, -6, -1600, -40});
    checkScore("no medium term selected: hard still folds x 10^9",
               scoreOf(defaultConstraints().only({"requiredSkillCost"}), s),
               Score{-1, 0, 0, 0});
    checkScore("level(Medium): minorPriorityTaskEndTimeCost moves onto soft0",
               scoreOf(defaultConstraints().level("minorPriorityTaskEndTimeCost",
                                                  optimaru::Level::Medium),
                       s),
               Score{-1, -42, -1600, 0});
    checkScore("a weight on a moved constraint is in medium units",
               scoreOf(defaultConstraints()
                           .level("minorPriorityTaskEndTimeCost",
                                  optimaru::Level::Medium)
                           .weight("minorPriorityTaskEndTimeCost", 0.5),
                       s),
               Score{-1, -22, -1600, 0});
    std::printf("\n");
}

// A random walk of list moves with the medium term overridden (promoted,
// demoted, re-weighted): the incremental score must equal a fresh full
// recompute of the same assembly after every move.
void verifyMediumIncremental()
{
    std::printf("== (g) Medium overrides: incremental == full ==\n");
    const std::vector<std::pair<std::string, std::function<ConstraintAssembly()>>>
        variants = {
            {"default", [] { return defaultConstraints(); }},
            {"hard()", [] { return defaultConstraints().hard("unassignedTasksCost"); }},
            {"soft()", [] { return defaultConstraints().soft("unassignedTasksCost"); }},
            {"weight 5", [] { return defaultConstraints().weight("unassignedTasksCost", 5); }},
            {"criticalPriorityTaskEndTimeCost on medium",
             []
             {
                 return defaultConstraints().level("criticalPriorityTaskEndTimeCost",
                                                   optimaru::Level::Medium);
             }},
        };
    for (const auto &variant : variants)
    {
        TaskAssigningSolution solution
            = buildRandomInstance(/*taskCount*/ 18, /*employeeCount*/ 4, 21);
        const int taskCount = static_cast<int>(solution.tasks.size());
        const int employeeCount = static_cast<int>(solution.employees.size());
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::ScoreDirector<TaskAssigningSolution> director(solution,
                                                                calculator);
        std::mt19937_64 rng(77);
        std::vector<int> taskList(taskCount, -1);
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 1500; ++step)
        {
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] >= 0)
            {
                const std::vector<int> &tasks
                    = solution.employees[taskList[task]].tasks;
                const int position = static_cast<int>(
                    std::find(tasks.begin(), tasks.end(), task) - tasks.begin());
                director.removeListElement(taskList[task], position);
                taskList[task] = -1;
            }
            if (rng() % 3 != 0)
            {
                const int list = static_cast<int>(rng() % employeeCount);
                const int position = static_cast<int>(
                    rng() % (solution.employees[list].tasks.size() + 1));
                director.insertListElement(list, position, task);
                taskList[task] = list;
            }
            const optimaru::HardSoftScore incremental = director.calculateScore();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            fresh.resetWorkingSolution(solution);
            const optimaru::HardSoftScore full = fresh.calculateScore();
            if (fresh.levelScores().medium != 0.0)
                ++mediumSteps;
            if (full.hard != incremental.hard
                || std::fabs(full.soft - incremental.soft) > 1e-6)
                ++mismatches;
        }
        check(mismatches == 0,
              variant.first + ": 1500 list moves, incremental == full");
        if (variant.first != "hard()" && variant.first != "soft()")
            check(mediumSteps > 0, variant.first + ": the walk visits medium states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraintCosts() — the raw per-unit (per-employee; one total for the
// global unassignedTasksCost) numbers before weight and level.
// -------------------------------------------------------------------------
std::vector<double> rawCosts(TaskAssigningSolution &solution,
                             const std::string &name)
{
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(solution);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        // e0 has s0; its task needs s0 + s1 + s2 -> 2 missing. e1 has
        // nothing; its task needs s3 -> 1 missing.
        Builder b;
        b.addEmployee({"s0"});
        b.addEmployee({});
        const int t0 = b.addTask(10, {"s0", "s1", "s2"});
        const int t1 = b.addTask(10, {"s3"});
        b.solution.employees[0].tasks = {t0};
        b.solution.employees[1].tasks = {t1};
        check(rawCosts(b.solution, "requiredSkillCost")
                  == std::vector<double>{2.0, 1.0},
              "requiredSkillCost: [2, 1] missing skills");
    }
    {
        // Every requirement covered; an unassigned task's skills do not count.
        Builder b;
        b.addEmployee({"s0", "s1"});
        const int t0 = b.addTask(10, {"s0", "s1"});
        b.addTask(10, {"s9"});  // unassigned
        b.solution.employees[0].tasks = {t0};
        check(rawCosts(b.solution, "requiredSkillCost")
                  == std::vector<double>{0.0},
              "requiredSkillCost: [0] when fully skilled");
    }
    {
        // 3 of 4 tasks unassigned -> one global total of 3.
        Builder b;
        b.addEmployee({});
        const int t0 = b.addTask(10);
        b.addTask(10);
        b.addTask(10);
        b.addTask(10);
        b.solution.employees[0].tasks = {t0};
        check(rawCosts(b.solution, "unassignedTasksCost")
                  == std::vector<double>{3.0},
              "unassignedTasksCost: [3] unassigned tasks");
        b.solution.employees[0].tasks = {0, 1, 2, 3};
        check(rawCosts(b.solution, "unassignedTasksCost")
                  == std::vector<double>{0.0},
              "unassignedTasksCost: [0] with every task assigned");
    }
    {
        // e0 (HIGH, x1): 10 min from 0, then 5 min with minStart 20 ->
        // start 20, end 25 -> 625. e1 has no task -> 0.
        Builder b;
        b.addEmployee({}, {Affinity::HIGH});
        b.addEmployee({});
        const int t0 = b.addTask(10);
        const int t1 = b.addTask(5, {}, Priority::MINOR, /*minStart*/ 20);
        b.solution.employees[0].tasks = {t0, t1};
        check(rawCosts(b.solution, "squaredMakespanCost")
                  == std::vector<double>{625.0, 0.0},
              "squaredMakespanCost: [25^2, 0]");
        b.solution.employees[0].tasks.clear();
        check(rawCosts(b.solution, "squaredMakespanCost")
                  == std::vector<double>{0.0, 0.0},
              "squaredMakespanCost: [0, 0] with empty lists");
    }
    {
        // e0 (HIGH, x1): CRITICAL 10 (end 10), MAJOR 10 (end 20), MINOR 10
        // (end 30), CRITICAL 10 (end 40); each rule sums its own end times.
        Builder b;
        b.addEmployee({}, {Affinity::HIGH});
        const int c0 = b.addTask(10, {}, Priority::CRITICAL);
        const int j0 = b.addTask(10, {}, Priority::MAJOR);
        const int n0 = b.addTask(10, {}, Priority::MINOR);
        const int c1 = b.addTask(10, {}, Priority::CRITICAL);
        b.solution.employees[0].tasks = {c0, j0, n0, c1};
        check(rawCosts(b.solution, "criticalPriorityTaskEndTimeCost")
                  == std::vector<double>{50.0},
              "criticalPriorityTaskEndTimeCost: [10 + 40]");
        check(rawCosts(b.solution, "majorPriorityTaskEndTimeCost")
                  == std::vector<double>{20.0},
              "majorPriorityTaskEndTimeCost: [20]");
        check(rawCosts(b.solution, "minorPriorityTaskEndTimeCost")
                  == std::vector<double>{30.0},
              "minorPriorityTaskEndTimeCost: [30]");
    }
    {
        // Only MINOR tasks on the list: the CRITICAL and MAJOR rules are 0;
        // an empty list zeroes the MINOR rule as well.
        Builder b;
        b.addEmployee({}, {Affinity::HIGH});
        const int n0 = b.addTask(10, {}, Priority::MINOR);
        b.addTask(10, {}, Priority::CRITICAL);  // unassigned
        b.addTask(10, {}, Priority::MAJOR);     // unassigned
        b.solution.employees[0].tasks = {n0};
        check(rawCosts(b.solution, "criticalPriorityTaskEndTimeCost")
                  == std::vector<double>{0.0},
              "criticalPriorityTaskEndTimeCost: [0] without CRITICAL tasks");
        check(rawCosts(b.solution, "majorPriorityTaskEndTimeCost")
                  == std::vector<double>{0.0},
              "majorPriorityTaskEndTimeCost: [0] without MAJOR tasks");
        b.solution.employees[0].tasks.clear();
        check(rawCosts(b.solution, "minorPriorityTaskEndTimeCost")
                  == std::vector<double>{0.0},
              "minorPriorityTaskEndTimeCost: [0] on an empty list");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog read off the default assembly is exactly the components'
// constants (units first, the global after: the list_constraints() order),
// and the level/weight guards hold on the domain's assembly.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool isGlobal = false)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable && !row.reward && row.isGlobal == isGlobal
           && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 6
              && rowIs<RequiredSkillCost>(rows[0], "requiredSkillCost", Level::Hard, 1.0)
              && rowIs<SquaredMakespanCost>(rows[1], "squaredMakespanCost", Level::Soft, 1e7)
              && rowIs<CriticalPriorityTaskEndTimeCost>(
                  rows[2], "criticalPriorityTaskEndTimeCost", Level::Soft, 4.0)
              && rowIs<MajorPriorityTaskEndTimeCost>(
                  rows[3], "majorPriorityTaskEndTimeCost", Level::Soft, 2.0)
              && rowIs<MinorPriorityTaskEndTimeCost>(
                  rows[4], "minorPriorityTaskEndTimeCost", Level::Soft, 1.0)
              && rowIs<UnassignedTasksCost>(
                  rows[5], "unassignedTasksCost", Level::Medium, 1.0, /*isGlobal*/ true),
          "info(): skills hard 1, makespan soft 1e7, priorities soft 4/2/1, "
          "unassigned medium 1 (global); all flippable, no reward, enabled");
    check(defaultConstraints().names()
              == std::vector<std::string>{"requiredSkillCost",
                                          "squaredMakespanCost",
                                          "criticalPriorityTaskEndTimeCost",
                                          "majorPriorityTaskEndTimeCost",
                                          "minorPriorityTaskEndTimeCost",
                                          "unassignedTasksCost"},
          "names(): units first, the global unassignedTasksCost last");
    check(!throwsInvalid([] { defaultConstraints().soft("requiredSkillCost"); })
              && !throwsInvalid([] { defaultConstraints().hard("unassignedTasksCost"); })
              && !throwsInvalid([] { defaultConstraints().soft("unassignedTasksCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("criticalPriorityTaskEndTimeCost"); }),
          "every level flips both ways (medium included)");
    check(throwsInvalid([] { defaultConstraints().weight("squaredMakespanCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().exponent("squaredMakespanCost", 0.0); }),
          "a non-positive exponent is refused");
    check(throwsInvalid([] { defaultConstraints().weight("requiredSkillCosts", 2.0); }),
          "a misspelled constraint name throws instead of doing nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Three HIGH-affinity employees: e0 (no skills) runs t0 (critical, 0..10,
// needs x) and t1 (major, 10..30), e1 runs t2 (minor, 0..5), e2 is idle.
// Raw per employee (e0, e1, e2): requiredSkillCost [1, 0, 0],
// squaredMakespanCost [900, 25, 0], critical [10, 0, 0], major [30, 0, 0],
// minor [0, 5, 0]; unassignedTasksCost [0].
TaskAssigningSolution repeatInstance()
{
    Builder b;
    for (int e = 0; e < 3; ++e)
        b.addEmployee({}, {Affinity::HIGH});
    const int t0 = b.addTask(10, {"x"}, Priority::CRITICAL);
    const int t1 = b.addTask(20, {}, Priority::MAJOR);
    const int t2 = b.addTask(5, {}, Priority::MINOR);
    b.solution.employees[0].tasks = {t0, t1};
    b.solution.employees[1].tasks = {t2};
    return b.solution;
}

optimaru::DecomposedScoreCalculator<TaskAssigningSolution>::LevelScores
levelsWith(ConstraintAssembly assembly, TaskAssigningSolution &s)
{
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.resetWorkingSolution(s);
    calculator.calculateScore();
    return calculator.levelScores();
}

ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("criticalPriorityTaskEndTimeCost", 30.0);
    assembly.addInstances(std::move(copyOf("squaredMakespanCost")
                                        .weight("squaredMakespanCost", 0.5)
                                        .offset("squaredMakespanCost", 1e6)
                                        .clip("squaredMakespanCost", false)));
    assembly.addInstances(std::move(copyOf("requiredSkillCost")
                                        .weight("requiredSkillCost", 2.0)
                                        .offset("requiredSkillCost", 1.0)
                                        .clip("requiredSkillCost", false)));
    assembly.addInstances(std::move(copyOf("unassignedTasksCost")
                                        .exponent("unassignedTasksCost", 2.0)
                                        .offset("unassignedTasksCost", 1.0)));
    assembly.addInstances(std::move(copyOf("unassignedTasksCost")
                                        .soft("unassignedTasksCost")
                                        .weight("unassignedTasksCost", 3.0)
                                        .offset("unassignedTasksCost", -2.0)
                                        .clip("unassignedTasksCost", false)));
    assembly.addInstances(
        std::move(copyOf("minorPriorityTaskEndTimeCost")
                      .exponent("minorPriorityTaskEndTimeCost", 2.0)
                      .offset("minorPriorityTaskEndTimeCost", 10.0)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        TaskAssigningSolution s = repeatInstance();
        ConstraintAssembly twice = copyOf("criticalPriorityTaskEndTimeCost");
        twice.addInstances(
            std::move(copyOf("criticalPriorityTaskEndTimeCost")
                          .weight("criticalPriorityTaskEndTimeCost", 0.5)
                          .exponent("criticalPriorityTaskEndTimeCost", 2.0)));
        DecomposedCalculator calculator;
        std::move(twice).into(calculator);
        calculator.resetWorkingSolution(s);
        calculator.calculateScore();
        const auto levels = calculator.levelScores();
        check(levels.hard == 0 && levels.medium == 0.0
                  && levels.soft == -(4.0 * 10 + 0.5 * 100),
              "4 x end + 0.5 x end^2 per employee: -90 soft");
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{10, 0, 0}
                  && !entries[0].scoped && entries[1].weight == 0.5
                  && entries[1].exponent == 2.0 && entries[1].defaultWeight == 4.0,
              "two instances, the same raw costs, each with its own knobs");
    }
    {
        TaskAssigningSolution s = repeatInstance();
        check(levelsWith(copyOf("criticalPriorityTaskEndTimeCost")
                             .offset("criticalPriorityTaskEndTimeCost", 4.0),
                         s)
                      .soft
                  == -4.0 * 6,
              "threshold offset 4: 4 x (10 - 4)");
        check(levelsWith(copyOf("criticalPriorityTaskEndTimeCost")
                             .offset("criticalPriorityTaskEndTimeCost", 4.0)
                             .clip("criticalPriorityTaskEndTimeCost", false),
                         s)
                      .soft
                  == -4.0 * (6 - 4 - 4),
              "literal offset 4: 4 x (6 - 4 - 4), the idle employee included");
        check(levelsWith(copyOf("majorPriorityTaskEndTimeCost")
                             .offset("majorPriorityTaskEndTimeCost", -1.0),
                         s)
                      .soft
                  == -2.0 * 31,
              "threshold offset -1 adds to the non-zero employee only");
        check(levelsWith(copyOf("requiredSkillCost")
                             .weight("requiredSkillCost", 3.0)
                             .offset("requiredSkillCost", 0.5)
                             .clip("requiredSkillCost", false),
                         s)
                      .hard
                  == 2,
              "required-skill literal offset 0.5 at weight 3: 1.5 -> 2, -1.5 -> "
              "-2 twice, +2 hard");
        const auto global = levelsWith(copyOf("unassignedTasksCost")
                                           .offset("unassignedTasksCost", 3.0)
                                           .clip("unassignedTasksCost", false),
                                       s);
        check(global.medium == 3.0 && global.hard == 0,
              "unassigned literal offset 3 on the single total: +3 medium");
    }
    {
        // Random LIST walk: incremental == a fresh calculator of the same
        // assembly.
        TaskAssigningSolution solution
            = buildRandomInstance(/*taskCount*/ 30, /*employeeCount*/ 6, 7);
        const int taskCount = static_cast<int>(solution.tasks.size());
        const int employeeCount = static_cast<int>(solution.employees.size());
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::ScoreDirector<TaskAssigningSolution> director(solution,
                                                                calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<int> taskList(taskCount, -1);
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
        int mediumSteps = 0;
        double drift = 0.0;
        const int steps = 2000;
        for (int step = 0; step < steps; ++step)
        {
            const int task = static_cast<int>(rng() % taskCount);
            const double dice = unit(rng);
            if (dice < 0.2 && taskList[task] >= 0)
            {
                director.removeListElement(taskList[task],
                                           indexWithin(taskList[task], task));
                taskList[task] = -1;
            }
            else if (dice < 0.85)
            {
                if (taskList[task] >= 0)
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
            TaskAssigningSolution shadow = solution;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.resetWorkingSolution(shadow);
            const optimaru::HardSoftScore full = fresh.calculateScore();
            if (fresh.levelScores().medium != 0.0)
                ++mediumSteps;
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scoresAgree(incremental, full))
                ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random list moves with repeated, shifted components: "
                    "incremental == fresh");
        check(mediumSteps > 0, "the walk visits medium states");
    }
    {
        TaskAssigningSolution solution
            = buildRandomInstance(/*taskCount*/ 20, /*employeeCount*/ 5, 11);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 5;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
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
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && solution.score == scoreWith(fresh, solution),
              "the reported score == a fresh recompute of the same assembly");
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
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All task-assigning plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d task-assigning plugin check(s) FAILED.\n", failures);
    return 1;
}
