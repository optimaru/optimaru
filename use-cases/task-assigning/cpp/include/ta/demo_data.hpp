#ifndef TA_DEMO_DATA_HPP
#define TA_DEMO_DATA_HPP

// Reproduction of the Java quickstart's DemoDataGenerator: 4 task types,
// 4 customers, 8 employees with skill sets and per-customer affinities, and
// 28 tasks. Shared by the demo executable and the tests.

#include <string>
#include <utility>
#include <vector>

#include "ta/domain.hpp"

namespace ta
{
namespace demo
{

// Skill names (DemoDataGenerator).
inline const char *const kProblemSolving = "Problem Solving";
inline const char *const kTeamBuilding = "Team Building";
inline const char *const kBusinessStorytelling = "Business Storytelling";
inline const char *const kRiskManagement = "Risk Management";
inline const char *const kCreativeThinking = "Creative Thinking";
inline const char *const kStrategicPlanning = "Strategic Planning";

// Task type indices.
enum : int
{
    kImproveSales = 0,  // IS
    kExpandTax = 1,     // ET
    kShrinkVat = 2,     // SV
    kApproveLegal = 3   // AL
};

// Customer indices.
enum : int
{
    kSteelInc = 0,
    kPaperCorp = 1,
    kStoneLimited = 2,
    kWoodExpress = 3
};

inline void addTask(TaskAssigningSolution &solution,
                    int taskType,
                    int indexInTaskType,
                    int customer,
                    Priority priority)
{
    Task task;
    task.id = std::to_string(solution.tasks.size() + 1);
    task.taskType = taskType;
    task.indexInTaskType = indexInTaskType;
    task.customer = customer;
    task.priority = priority;
    solution.tasks.push_back(task);
}

inline void addEmployee(TaskAssigningSolution &solution,
                        const std::string &fullName,
                        std::vector<std::string> skills,
                        std::vector<Affinity> affinityByCustomer)
{
    Employee employee;
    employee.id = std::to_string(solution.employees.size() + 1);
    employee.fullName = fullName;
    employee.skills = std::move(skills);
    employee.affinityByCustomer = std::move(affinityByCustomer);
    solution.employees.push_back(employee);
}

inline TaskAssigningSolution generateDemoData()
{
    TaskAssigningSolution solution;

    solution.taskTypes
        = {{"IS", "Improve Sales", 46, {kStrategicPlanning}},
           {"ET", "Expand Tax", 63, {kProblemSolving, kTeamBuilding}},
           {"SV", "Shrink VAT", 63, {kStrategicPlanning}},
           {"AL", "Approve Legal", 40, {kRiskManagement}}};

    solution.customers = {{"1", "Steel Inc"},
                          {"2", "Paper Corp"},
                          {"3", "Stone Limited"},
                          {"4", "Wood Express"}};

    // Employees (affinities ordered Steel, Paper, Stone, Wood).
    using A = Affinity;
    addEmployee(solution,
                "Amy",
                {kProblemSolving, kBusinessStorytelling, kTeamBuilding},
                {A::HIGH, A::MEDIUM, A::HIGH, A::MEDIUM});
    addEmployee(solution,
                "Beth",
                {kRiskManagement, kCreativeThinking},
                {A::LOW, A::HIGH, A::LOW, A::MEDIUM});
    addEmployee(solution,
                "Carl",
                {kStrategicPlanning, kProblemSolving},
                {A::MEDIUM, A::HIGH, A::MEDIUM, A::LOW});
    addEmployee(solution,
                "Dan",
                {kBusinessStorytelling, kTeamBuilding},
                {A::LOW, A::HIGH, A::HIGH, A::MEDIUM});
    addEmployee(solution,
                "Elsa",
                {kRiskManagement, kStrategicPlanning, kCreativeThinking},
                {A::MEDIUM, A::HIGH, A::HIGH, A::MEDIUM});
    addEmployee(solution,
                "Flo",
                {kProblemSolving, kTeamBuilding},
                {A::HIGH, A::HIGH, A::NONE, A::MEDIUM});
    addEmployee(solution,
                "Gus",
                {kRiskManagement, kCreativeThinking, kBusinessStorytelling},
                {A::LOW, A::MEDIUM, A::MEDIUM, A::LOW});
    addEmployee(solution,
                "Hugo",
                {kStrategicPlanning, kProblemSolving, kTeamBuilding},
                {A::NONE, A::MEDIUM, A::NONE, A::LOW});

    // Tasks 1..28 (DemoDataGenerator.generateTasks, same order).
    addTask(solution, kShrinkVat, 1, kPaperCorp, Priority::MINOR);
    addTask(solution, kApproveLegal, 1, kWoodExpress, Priority::MAJOR);
    addTask(solution, kShrinkVat, 2, kWoodExpress, Priority::MAJOR);
    addTask(solution, kImproveSales, 1, kWoodExpress, Priority::CRITICAL);
    addTask(solution, kApproveLegal, 2, kStoneLimited, Priority::MINOR);
    addTask(solution, kApproveLegal, 3, kStoneLimited, Priority::MINOR);
    addTask(solution, kExpandTax, 1, kPaperCorp, Priority::MINOR);
    addTask(solution, kExpandTax, 2, kSteelInc, Priority::MINOR);
    addTask(solution, kExpandTax, 3, kSteelInc, Priority::MINOR);
    addTask(solution, kShrinkVat, 3, kWoodExpress, Priority::MAJOR);
    addTask(solution, kImproveSales, 2, kWoodExpress, Priority::MINOR);
    addTask(solution, kExpandTax, 4, kSteelInc, Priority::MAJOR);
    addTask(solution, kImproveSales, 3, kStoneLimited, Priority::MINOR);
    addTask(solution, kExpandTax, 5, kWoodExpress, Priority::MAJOR);
    addTask(solution, kExpandTax, 6, kStoneLimited, Priority::CRITICAL);
    addTask(solution, kExpandTax, 7, kPaperCorp, Priority::MINOR);
    addTask(solution, kApproveLegal, 4, kStoneLimited, Priority::MAJOR);
    addTask(solution, kApproveLegal, 5, kSteelInc, Priority::MAJOR);
    addTask(solution, kImproveSales, 4, kWoodExpress, Priority::MAJOR);
    addTask(solution, kImproveSales, 5, kWoodExpress, Priority::CRITICAL);
    addTask(solution, kImproveSales, 6, kSteelInc, Priority::MINOR);
    addTask(solution, kImproveSales, 7, kPaperCorp, Priority::MAJOR);
    addTask(solution, kImproveSales, 8, kWoodExpress, Priority::CRITICAL);
    addTask(solution, kApproveLegal, 6, kWoodExpress, Priority::MINOR);
    addTask(solution, kApproveLegal, 7, kSteelInc, Priority::CRITICAL);
    addTask(solution, kImproveSales, 9, kWoodExpress, Priority::MAJOR);
    addTask(solution, kImproveSales, 10, kSteelInc, Priority::CRITICAL);
    addTask(solution, kImproveSales, 11, kWoodExpress, Priority::MAJOR);

    return solution;
}

}  // namespace demo
}  // namespace ta

#endif  // TA_DEMO_DATA_HPP
