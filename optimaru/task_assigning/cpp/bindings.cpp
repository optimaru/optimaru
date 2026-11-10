// pybind11 bindings for the task-assigning C++ domain wrappers.
// The Python extension links the existing ta::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
// is not modified here. One domain is exposed: ta::TaskAssigningSolution
// (sequence shape — every employee holds an ordered task list; a task in
// no list is unassigned), as the module
// optimaru.task_assigning._core.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string

#include <chrono>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ta/constraints.hpp"
#include "ta/domain.hpp"
#include "ta/score_calculator.hpp"
#include "ta/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace ta;
namespace opy = optimaru::python;

namespace {

// Referential integrity checks shared by the solution constructor and
// set_task_lists: broken indices would otherwise crash the C++ engine.
void validateFacts(const TaskAssigningSolution& solution) {
    const int typeCount = static_cast<int>(solution.taskTypes.size());
    const int customerCount = static_cast<int>(solution.customers.size());
    for (const Task& task : solution.tasks) {
        if (task.taskType < 0 || task.taskType >= typeCount)
            throw py::value_error("task " + task.id + ": task_type index out of range");
        if (task.customer < -1 || task.customer >= customerCount)
            throw py::value_error("task " + task.id + ": customer index out of range");
    }
}

void validateTaskLists(const std::vector<std::vector<int>>& lists, int taskCount) {
    std::vector<int> seen(taskCount, 0);
    for (const std::vector<int>& list : lists) {
        for (int taskIndex : list) {
            if (taskIndex < 0 || taskIndex >= taskCount)
                throw py::value_error("task index " + std::to_string(taskIndex) + " out of range");
            if (++seen[taskIndex] > 1)
                throw py::value_error("task index " + std::to_string(taskIndex) +
                                      " appears in more than one task list");
        }
    }
}

void validateTaskLists(const TaskAssigningSolution& solution) {
    std::vector<std::vector<int>> lists;
    lists.reserve(solution.employees.size());
    for (const Employee& employee : solution.employees) lists.push_back(employee.tasks);
    validateTaskLists(lists, static_cast<int>(solution.tasks.size()));
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Task assigning (skill-based task queues per employee) — "
            "optimaru engine-backed C++ wrapper exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
            .def_readonly("hard", &Score::hard)
            .def_readonly("soft0", &Score::soft0)
            .def_readonly("soft1", &Score::soft1)
            .def_readonly("soft2", &Score::soft2)
            .def("is_feasible", &Score::feasible)
            .def("__str__", &Score::str)
            .def("__repr__", [](const Score& s) { return "<Score " + s.str() + ">"; });

    py::enum_<Affinity>(m, "Affinity", py::module_local())
            .value("NONE", Affinity::NONE)
            .value("LOW", Affinity::LOW)
            .value("MEDIUM", Affinity::MEDIUM)
            .value("HIGH", Affinity::HIGH);

    py::enum_<Priority>(m, "Priority", py::module_local())
            .value("MINOR", Priority::MINOR)
            .value("MAJOR", Priority::MAJOR)
            .value("CRITICAL", Priority::CRITICAL);

    py::class_<Customer>(m, "Customer", py::module_local())
            .def(py::init([](std::string id, std::string name) {
                     Customer c;
                     c.id = std::move(id);
                     c.name = std::move(name);
                     return c;
                 }),
                 py::arg("id"), py::arg("name") = "")
            .def_readwrite("id", &Customer::id)
            .def_readwrite("name", &Customer::name);

    py::class_<TaskType>(m, "TaskType", py::module_local())
            .def(py::init([](std::string code, std::string title, std::int64_t baseDuration,
                             std::vector<std::string> requiredSkills) {
                     TaskType t;
                     t.code = std::move(code);
                     t.title = std::move(title);
                     t.baseDuration = baseDuration;
                     t.requiredSkills = std::move(requiredSkills);
                     return t;
                 }),
                 py::arg("code"), py::arg("title") = "", py::arg("base_duration") = 0,
                 py::arg("required_skills") = std::vector<std::string>{})
            .def_readwrite("code", &TaskType::code)
            .def_readwrite("title", &TaskType::title)
            // Minutes.
            .def_readwrite("base_duration", &TaskType::baseDuration)
            .def_readwrite("required_skills", &TaskType::requiredSkills);

    py::class_<Task>(m, "Task", py::module_local())
            .def(py::init([](std::string id, int taskType, int indexInTaskType, int customer,
                             std::int64_t minStartTime, Priority priority) {
                     Task t;
                     t.id = std::move(id);
                     t.taskType = taskType;
                     t.indexInTaskType = indexInTaskType;
                     t.customer = customer;
                     t.minStartTime = minStartTime;
                     t.priority = priority;
                     return t;
                 }),
                 py::arg("id"), py::arg("task_type"), py::arg("index_in_task_type") = 0,
                 py::arg("customer") = -1, py::arg("min_start_time") = 0,
                 py::arg("priority") = Priority::MINOR)
            .def_readwrite("id", &Task::id)
            // Index into TaskAssigningSolution.task_types.
            .def_readwrite("task_type", &Task::taskType)
            .def_readwrite("index_in_task_type", &Task::indexInTaskType)
            // Index into TaskAssigningSolution.customers (-1 == no customer,
            // treated as Affinity.NONE).
            .def_readwrite("customer", &Task::customer)
            .def_readwrite("min_start_time", &Task::minStartTime)
            .def_readwrite("priority", &Task::priority)
            // Derived fields, computed from the employee task lists by
            // calculate_score / Solver.solve: employee -1 == unassigned,
            // start_time -1 == unassigned.
            .def_readonly("employee", &Task::employee)
            .def_readonly("start_time", &Task::startTime);

    py::class_<Employee>(m, "Employee", py::module_local())
            .def(py::init([](std::string id, std::string fullName, std::vector<std::string> skills,
                             std::vector<Affinity> affinityByCustomer, std::vector<int> tasks) {
                     Employee e;
                     e.id = std::move(id);
                     e.fullName = std::move(fullName);
                     e.skills = std::move(skills);
                     e.affinityByCustomer = std::move(affinityByCustomer);
                     e.tasks = std::move(tasks);
                     return e;
                 }),
                 py::arg("id"), py::arg("full_name") = "",
                 py::arg("skills") = std::vector<std::string>{},
                 py::arg("affinity_by_customer") = std::vector<Affinity>{},
                 py::arg("tasks") = std::vector<int>{})
            .def_readwrite("id", &Employee::id)
            .def_readwrite("full_name", &Employee::fullName)
            .def_readwrite("skills", &Employee::skills)
            // Affinity per customer index; missing entries mean Affinity.NONE.
            .def_readwrite("affinity_by_customer", &Employee::affinityByCustomer)
            // The decision variable: ordered task indices. NOTE: employees
            // read from a solution are copies; use
            // TaskAssigningSolution.set_task_lists for bulk writes.
            .def_readwrite("tasks", &Employee::tasks);

    py::class_<TaskAssigningSolution>(m, "TaskAssigningSolution", py::module_local())
            .def(py::init([](std::vector<TaskType> taskTypes, std::vector<Customer> customers,
                             std::vector<Task> tasks, std::vector<Employee> employees) {
                     TaskAssigningSolution s;
                     s.taskTypes = std::move(taskTypes);
                     s.customers = std::move(customers);
                     s.tasks = std::move(tasks);
                     s.employees = std::move(employees);
                     validateFacts(s);
                     validateTaskLists(s);
                     return s;
                 }),
                 py::arg("task_types"), py::arg("customers"), py::arg("tasks"),
                 py::arg("employees"))
            .def_readonly("task_types", &TaskAssigningSolution::taskTypes)
            .def_readonly("customers", &TaskAssigningSolution::customers)
            // NOTE: element access returns copies; the assignment lives in
            // the employees' task lists (use task_lists / set_task_lists).
            .def_readonly("tasks", &TaskAssigningSolution::tasks)
            .def_readonly("employees", &TaskAssigningSolution::employees)
            .def_readonly("score", &TaskAssigningSolution::score)
            // Convenience: the ordered task-index list per employee (the
            // decision variable of each employee).
            .def("task_lists",
                 [](const TaskAssigningSolution& solution) {
                     std::vector<std::vector<int>> lists;
                     lists.reserve(solution.employees.size());
                     for (const Employee& employee : solution.employees)
                         lists.push_back(employee.tasks);
                     return lists;
                 })
            // Bulk write of the task lists (employee access via
            // .employees returns copies, so per-employee writes would be lost).
            .def(
                    "set_task_lists",
                    [](TaskAssigningSolution& solution,
                       const std::vector<std::vector<int>>& lists) {
                        if (lists.size() != solution.employees.size())
                            throw py::value_error(
                                    "task_lists length must equal the employee count");
                        // Validate BEFORE mutating, so a rejected write leaves the
                        // solution untouched.
                        validateTaskLists(lists, static_cast<int>(solution.tasks.size()));
                        for (std::size_t e = 0; e < lists.size(); ++e)
                            solution.employees[e].tasks = lists[e];
                    },
                    py::arg("task_lists"));

    m.def("calculate_score", &ScoreCalculator::score, py::arg("solution"),
          "Full recompute of the four-level score at the default weights "
          "(recomputes the derived task fields and stores solution.score).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("relocate_move_weight", &Solver::Config::relocate_weight)
            .def_readwrite("list_swap_move_weight", &Solver::Config::list_swap_weight)
            .def_readwrite("two_opt_move_weight", &Solver::Config::two_opt_weight)
            .def_readwrite("or_opt_move_weight", &Solver::Config::or_opt_weight)
            .def_readwrite("tail_swap_move_weight", &Solver::Config::tail_swap_weight)
            // Early termination once bestScore >= (hard, soft0, soft1, soft2),
            // e.g. (0, 0, -2**63, -2**63) stops at the first feasible,
            // fully-assigned solution.
            .def_property(
                    "best_score_limit",
                    [](const Solver::Config& c) {
                        return py::make_tuple(c.target_score.hard, c.target_score.soft0,
                                              c.target_score.soft1, c.target_score.soft2);
                    },
                    [](Solver::Config& c,
                       std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t> levels) {
                        c.target_score = Score{std::get<0>(levels), std::get<1>(levels),
                                               std::get<2>(levels), std::get<3>(levels)};
                    });

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: reorders employees[*].tasks, recomputes the derived
            // task fields and sets solution.score. The GIL is released so
            // parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<TaskAssigningSolution&>(&Solver::solve),
                 py::arg("solution"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.task_assigning.constraints). The picks replace the defaults; None
            // keeps them. A name may be picked several times: every pick is its
            // own instance (a fresh copy of the default component) with its own
            // knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, TaskAssigningSolution& solution,
                       const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(solution, calculator);
                    },
                    py::arg("solution"), py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `solution` — the input
    // optimaru.task_assigning.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // Every entry carries the domain's medium fold ("medium_fold":
    // {"kind": "below_hard", "unit": 1e9}), so score() folds like the engine.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
            "constraint_costs",
            [](TaskAssigningSolution& solution, const py::object& constraints) {
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                return opy::constraint_costs(calculator, solution);
            },
            py::arg("solution"), py::arg("constraints") = py::none(),
            "Raw per-unit cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
            "list_constraints", [] { return opy::constraint_names(defaultConstraints()); },
            "Constraint names available to optimaru.task_assigning.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    opy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
