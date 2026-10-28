import pytest

import task_assigning as domain
from task_assigning.constraints import get_constraints
from task_assigning.demo import generate_demo_data
from task_assigning.models import (
    CustomerModel,
    EmployeeModel,
    TaskAssigningSolutionModel,
    TaskModel,
    TaskTypeModel,
)
from task_assigning import calculate_score
from task_assigning_cpsat import catalog, solve


def test_smoke_and_plugin_catalog():
    problem = generate_demo_data()
    result = solve(problem, time_limit_seconds=2.0)
    assert len(
        {task for employee in result.employees for task in employee.tasks}
    ) == len(problem.tasks)
    assert result.score_hard == 0
    assert {entry["name"] for entry in catalog()} >= {
        "no_missing_skills",
        "minimize_unassigned_tasks",
    }
    shared = calculate_score(result.to_core())
    assert (
        result.score_hard,
        result.score_soft0,
        result.score_soft1,
        result.score_soft2,
    ) == (shared.hard, shared.soft0, shared.soft1, shared.soft2)


def test_explicit_constraint_list_replaces_defaults():
    result = solve(
        generate_demo_data(),
        time_limit_seconds=2.0,
        constraints=["minimize_unassigned_tasks"],
    )
    assert result.score_hard == 0
    assert result.score_soft1 == 0
    assert result.score_soft2 == 0


def _parity_problem() -> TaskAssigningSolutionModel:
    return TaskAssigningSolutionModel(
        task_types=[
            TaskTypeModel(code="T", base_duration=10, required_skills=["required"])
        ],
        customers=[CustomerModel(id="C")],
        tasks=[
            TaskModel(
                id="critical", task_type_code="T", customer_id="C", priority="CRITICAL"
            ),
            TaskModel(
                id="major", task_type_code="T", customer_id="C", priority="MAJOR"
            ),
            TaskModel(
                id="minor", task_type_code="T", customer_id="C", priority="MINOR"
            ),
            TaskModel(
                id="unassigned",
                task_type_code="T",
                customer_id="C",
                priority="MINOR",
            ),
        ],
        employees=[EmployeeModel(id="E", skills=[], tasks=[])],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_tasks(constraint) -> None:
    problem = _parity_problem()
    employees = [0, 0, 0, -1]
    starts = [0, 40, 80, 0]
    candidate = problem.model_copy(deep=True)
    candidate.employees[0].tasks = ["critical", "major", "minor"]
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_employee_by_task=employees,
        _fixed_starts=starts,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)
